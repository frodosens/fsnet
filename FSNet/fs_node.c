    //
//  fs_node.c
//  fsnet
//
//  Created by Vincent on 14-5-20.
//  Copyright (c) 2014年 Vincent. All rights reserved.
//

#include <string.h>
#include <errno.h>
#include <event.h>
#include <pthread.h>
#include <unistd.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include "fs_node.h"
#include "fs_define.h"
#include "fs_malloc.h"
#include "fs_struct.h"
#include "fs_server.h"
#include "fs_stream.h"

struct fs_node{
    
    fs_id           node_id;
    char            node_name[64];
    int             socket;
    struct event*   read_ev;
    struct event*   write_ev;
    struct fs_server* server;
    struct fs_output_stream* recv_buffer;
    struct fs_output_stream* send_buffer;
    pthread_mutex_t write_mutex;
    pthread_mutex_t close_mutex;
    fs_bool         closed;
    fs_script_id    script_id;
    
} ;

#define RECV_BUFF_MAX 1024
void libevent_cb_node_onrecv_data(int socket, short event, void* arg);
void libevent_cb_node_onsend_data(int socket, short event, void* arg);


struct fs_node*
fs_create_node(struct fs_server* server){
    
    struct fs_node* ret = (struct fs_node*)fs_malloc(sizeof(*ret));
    fs_zero(ret, sizeof(*ret));
    ret->server = server;
    
    return ret;
}

fs_bool
fs_node_bind_event(struct fs_node* node,
                   fs_id node_id,
                   int socket,
                   struct sockaddr* sa,
                   socklen_t socklen,
                   struct fs_server* server,
                   struct event_base* event_base ){
    
    int ret = 0;
    if(node->read_ev){
        event_free(node->read_ev);
    }
    if(node->write_ev){
        event_free(node->write_ev);
    }
    node->node_id = node_id;
    node->socket = socket;
    node->server = server;
    node->read_ev = (struct event*)fs_malloc(sizeof(struct event));
    node->write_ev = (struct event*)fs_malloc(sizeof(struct event));
    
    const char bOper = 1;
    evutil_make_socket_nonblocking(node->socket);
    setsockopt(socket, IPPROTO_TCP, TCP_NODELAY, (const char*)&bOper, sizeof(const char));
    
    ret = event_assign(node->read_ev, event_base,
                       node->socket, EV_READ | EV_PERSIST,
                       libevent_cb_node_onrecv_data, node);
    
    ret = event_assign(node->write_ev, event_base,
                       node->socket, EV_WRITE | EV_PERSIST,
                       libevent_cb_node_onsend_data, node);
    
    
    
    if(ret != 0){
        goto fail;
    }
    
    node->recv_buffer = fs_create_output_stream_ext;
    node->send_buffer = fs_create_output_stream_ext;
    
    pthread_mutex_init(&node->write_mutex, NULL);
    pthread_mutex_init(&node->close_mutex, NULL);
    event_add(node->read_ev, NULL);
    event_add(node->write_ev, NULL);
    
    goto success;
    
success:
    return fs_true;
fail:
    if(node->read_ev) event_free(node->read_ev);
    if(node->write_ev) event_free(node->write_ev);
    return fs_false;
}


fs_bool
fs_node_is_closed( struct fs_node* node ){
    
    pthread_mutex_lock(&node->close_mutex);
    fs_bool ret = node->closed;
    pthread_mutex_unlock(&node->close_mutex);
    return ret;
    
}

void
fs_node_set_closed( struct fs_node* node ){
    
    pthread_mutex_lock(&node->close_mutex);
    node->closed = fs_true;
    pthread_mutex_unlock(&node->close_mutex);
    
}


void
fs_node_close(struct fs_node* node){
    
    
    pthread_mutex_lock(&node->close_mutex);
    
    if(node->closed) return;
    node->closed = fs_true;
    
    if(node->recv_buffer){
        fs_stream_free_output(node->recv_buffer);
        node->recv_buffer = NULL;
    }
    
    if(node->send_buffer){
        pthread_mutex_lock(&node->write_mutex);
        fs_stream_free_output(node->send_buffer);
        node->send_buffer = NULL;
        pthread_mutex_unlock(&node->write_mutex);
    }
    pthread_mutex_destroy(&node->write_mutex);
    
    fs_node_close_socket(node);
    
    if(node->read_ev != NULL){
        event_free(node->read_ev);
        node->read_ev = NULL;
    }
    if(node->write_ev != NULL){
        event_free(node->write_ev);
        node->write_ev = NULL;
    }
    if(fs_node_is_from_listener(node)){
        fs_server_on_node_shudown(node->server, node->node_id);
    }
    
    pthread_mutex_unlock(&node->close_mutex);
    pthread_mutex_destroy(&node->close_mutex);
    

}


void
fs_node_shudown( struct fs_node* node ){
    if( ! fs_server_close_node(node->server, node->node_id) ){
        fs_node_close(node);
    }
}

void
fs_node_close_socket(struct fs_node* node){
    if(node->socket != 0){
        evutil_closesocket(node->socket);
        node->socket = 0;
    }
}

void
fs_free_node(struct fs_node* node){
    
    fs_node_close(node);

}

fs_id
fs_node_get_id(struct fs_node* node){
    return node->node_id;
}


fs_bool
fs_node_is_active( struct fs_node* node){
    return node->socket != 0;
}

fs_bool
fs_node_is_from_connect(struct fs_node* node){
    return (node->node_id & 0xff000000) != 0;
}
fs_bool
fs_node_is_from_listener(struct fs_node* node){
    return (node->node_id & 0x00ffffff) != 0;
}



void
fs_node_recv_data(struct fs_node* node, BYTE* data, size_t len){
    
    if(!node->recv_buffer){
        fprintf(stderr, "fs_node_recv_data but node recvbuffer is NULL");
        return ;
    }
    
    fs_stream_write_data(node->recv_buffer, data, len);
    
    const BYTE* stream_data = fs_output_stream_get_dataptr(node->recv_buffer);
    size_t stream_len = fs_output_stream_get_len(node->recv_buffer);
    
    struct fs_pack* pack = NULL;
    size_t offset = 0;
    do{
        pack = NULL;
        if(node->server != NULL){
            size_t pack_len = fs_server_parse_pack(node->server, stream_data + offset, stream_len - offset, node->node_id, &pack);
            if(pack_len){
                fs_server_on_recv_pack(node->server, pack);
            }
            offset += pack_len;
        }
        
    }while (pack != NULL);
    
    if(offset > 0 && node->recv_buffer){
        fs_output_stream_sub(node->recv_buffer, offset, stream_len - offset);
    }
}


void
fs_node_send_data(struct fs_node* node, BYTE* data, size_t len){
    
    if(fs_node_is_closed(node)){
        fprintf(stderr, "Try to an closed node[%d] to send data", node->node_id);
        return;
    }
    
    pthread_mutex_lock(&node->write_mutex);
    if(!node->send_buffer){
        fprintf(stderr, "Try to an unreachable node[%d] to send data", node->node_id);
        pthread_mutex_unlock(&node->write_mutex);
        return;
    }
    
    fs_stream_write_data(node->send_buffer, data, len);
    
    pthread_mutex_unlock(&node->write_mutex);
    
    if(!event_pending(node->write_ev, EV_WRITE, NULL)){
        event_add(node->write_ev, NULL);
    }
    
}





void
libevent_cb_node_onsend_data(int socket, short event, void* arg){
    
    struct fs_node* node = (struct fs_node*)arg;
    
    pthread_mutex_lock(&node->write_mutex);
    
    if(node->send_buffer == NULL)
    {
        event_del(node->write_ev);
        pthread_mutex_unlock(&node->write_mutex);
        return;
    }
    
    void* data = (void*)fs_output_stream_get_dataptr(node->send_buffer);
    size_t len = fs_output_stream_get_len(node->send_buffer);
    ssize_t nsize = len;
    ssize_t nwrite = 0;
    ssize_t nwrited = 0;
    while (nsize > 0) {
        nwrite = send(node->socket, (BYTE*)data + (len - nsize), nsize, 0);
        if(nwrite > 0){
            nwrited += nwrite;
        }
        if(nwrite < nsize){
            if(nwrite == -1){
                if(errno == EAGAIN)
                {
                    break;
                }else{
                    fprintf(stderr, "%d on send_data error = %d \n", node->node_id, errno);
                    
                    pthread_mutex_unlock(&node->write_mutex);
                    fs_node_shudown(node);
                    return;
                }
            }
        }
        nsize -= nwrite;
    }
    
    fs_output_stream_sub(node->send_buffer, nwrited, len - nwrited);

    
    len = fs_output_stream_get_len(node->send_buffer);
    if(len == 0){
        event_del(node->write_ev);
    }
    
    pthread_mutex_unlock(&node->write_mutex);


}

void
libevent_cb_node_onrecv_data(int socket, short event, void* arg){
    struct fs_node* node = (struct fs_node*)arg;
    
    ssize_t nread  = 0;
    BYTE buff[RECV_BUFF_MAX];
    
    
    do{
        fs_zero(buff, RECV_BUFF_MAX);
        nread = recv(socket, buff, RECV_BUFF_MAX, 0);
        
        
        if(nread > 0){
            fs_node_recv_data(node, buff, nread);
        }else{
            if(nread == -1){
                if(errno == EAGAIN){
                    break;
                }else{
                    fprintf(stderr, "%d on recv_data error[%d] \n", node->node_id, errno);
                    fs_node_shudown(node);
                    return;
                }
            }else{
                fs_node_shudown(node);
                return;
            }
        }
        
        
    } while (fs_true);
    
    
}






void
fs_node_set_script_id( struct fs_node* node, fs_script_id _id ){
    node->script_id = _id;
}


fs_script_id
fs_node_get_script_id( struct fs_node* node){
    return node->script_id;
}











