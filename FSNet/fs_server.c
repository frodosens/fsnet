//
//  fs_server.c
//  fsnet
//
//  Created by Vincent on 14-5-20.
//  Copyright (c) 2014年 Vincent. All rights reserved.
//

#include <stdio.h>
#include <string.h>
#include <event.h>
#include <evhttp.h>
#include <event2/listener.h>
#include <pthread.h>
#include <signal.h>

#include <unistd.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <net/if.h>
#include <arpa/inet.h>


#include "fs_server.h"
#include "fs_malloc.h"
#include "fs_define.h"
#include "fs_struct.h"
#include "fs_loop_queue.h"
#include "fs_node.h"
#include "fs_pack.h"

#include "hash.h"


#define LOOP_QUE_LEN 1024
struct fs_server{
    
    char name[64];
    
    int socket;
    
    fs_bool running;
    
    fs_bool create_work_thread;
    
    enum fs_server_type server_type;
    
    
    pthread_t pthread_io_id;
    pthread_t pthread_work_id;
    
    pthread_mutex_t pthread_work_mutex;
    pthread_cond_t pthread_work_cond;
    
    uint32_t listener_node_serial_id;
    uint32_t connect_node_serial_id;
    
    struct event_base* event;
    struct event*      signal_event;
    
    struct fs_node_addr addr;
    struct fs_loop_queue* loopque;
    
    
    struct fs_loop_queue* event_loopque;
    
    hash_map node_map;
    
    fn_fs_pack_handle  _fn_pack_handle;
    fn_fs_pack_parse   _fn_pack_parse;
    fn_fs_pack_to_data _fn_pack_to_data;
    fn_fs_server_on_start _fn_on_start;
    fn_fs_node_connect _fn_node_connect;
    fn_fs_node_shudown _fn_node_shudown;
    
    fs_script_id script_id;
    
    
    fs_bool            need_pop_event;  // 是否需要通知新事件
};

fs_id
fs_server_next_listener_id(struct fs_server* server){
    return (++server->listener_node_serial_id) & 0x00ffffff;
}

fs_id
fs_server_next_connect_id(struct fs_server* server){
    return (++server->connect_node_serial_id) << 24;
}

static void
fs_server_add_node(struct fs_server* server, fs_id node_id, struct fs_node* node){
    char key[64];
    snprintf(key, 64, "%d", node_id);
    hmap_insert(server->node_map, key, -1, node);
}

static void
fs_server_rm_node(struct fs_server* server, fs_id node_id){
    char key[64];
    snprintf(key, 64, "%d", node_id);
    fs_server_add_node(server, node_id, NULL);
}


static void
libevent_cb_signal( evutil_socket_t fd, short e, void* data ){
    
    struct fs_server* server = (struct fs_server*)data;
    struct event* event = NULL;
    event = NULL;
    do{
        event = fs_loop_queue_pop(server->event_loopque);
        if(event && event != (void*)1){
            event_add(event, NULL);
        }
    }while( event != NULL );
    server->need_pop_event = fs_true;
    
}

static void
libevent_cb_listener(struct evconnlistener *listener, evutil_socket_t fd,
                      struct sockaddr *sa, int socklen, void *user_data){

    struct fs_server* server = (struct fs_server*)user_data;
    
    fs_id node_id = fs_server_next_listener_id(server);
    
    struct fs_node* node = fs_create_node(server);
    
    fs_node_bind_event(node, node_id, fd, sa, socklen, server, server->event);
    
    fs_server_add_node(server, node_id, node);
    
    if(server->_fn_node_connect){
        server->_fn_node_connect(server, node_id);
    }
    
    
    
}


static void*
fs_server_work_thread(void* data){
    
    struct fs_server* server = (struct fs_server*)data;
    
    while (server->running) {
        
        if(!fs_server_handle_pack(server)){
            
            pthread_cond_wait(&server->pthread_work_cond, &server->pthread_work_mutex);
        
        }
        
    }
    
    
    return NULL;
}

static void
fs_server_http_request(struct evhttp_request *req, void *data){
    struct fs_server* server = (struct fs_server*)data;
    const char* uri = evhttp_request_uri(req);
    enum evhttp_cmd_type type = evhttp_request_get_command(req);
    char *post_data = (char *) EVBUFFER_DATA(req->input_buffer);
    struct evkeyvalq params;
    struct evkeyvalq* heads = evhttp_request_get_input_headers(req);
    uint16_t params_count = 0;
    uint16_t heads_count = 0;
    
    struct fs_output_stream* fos = fs_create_output_stream_ext;
    
    fs_stream_write_uint64(fos, (uint64_t)req);
    fs_stream_write_c_string(fos, type == EVHTTP_REQ_POST ? "POST" : "GET");
    fs_stream_write_string(fos, uri, strlen(uri));
    
    if(post_data){
        size_t pos_data_len = EVBUFFER_LENGTH(req->input_buffer);
        fs_stream_write_uint16(fos, pos_data_len);
        fs_stream_write_data(fos, (void*)post_data, pos_data_len);
        char msg[pos_data_len + 1];
        msg[pos_data_len] = '\0';
        strncpy(msg, post_data, pos_data_len);
        evhttp_parse_query_str(msg, &params);
        
    }else{
        fs_stream_write_uint16(fos, 0);
        evhttp_parse_query(uri, &params);
    }
    
    struct evkeyval* header;
	TAILQ_FOREACH(header, &params, next) {
        params_count ++;
    }
    
	TAILQ_FOREACH(header, heads, next) {
        heads_count ++;
    }
    
    fs_stream_write_uint16(fos, params_count);
	TAILQ_FOREACH(header, &params, next) {
        fs_stream_write_string(fos, header->key, strlen(header->key));
        fs_stream_write_string(fos, header->value, strlen(header->value));
    }
    
    fs_stream_write_uint16(fos, heads_count);
	TAILQ_FOREACH(header, heads, next) {
        fs_stream_write_string(fos, header->key, strlen(header->key));
        fs_stream_write_string(fos, header->value, strlen(header->value));
    }
    
    if(server->_fn_pack_parse){
        struct fs_pack* out = NULL;
        server->_fn_pack_parse(server, fs_output_stream_get_dataptr(fos), fs_output_stream_get_len(fos), 0, &out);
        fs_server_need_work(server);
    }
    fs_stream_free_output(fos);
    
}


static void*
fs_server_io_thread(void* data){

    struct fs_server* server = (struct fs_server*)data;
    
    fs_assert(server != NULL);
    
    sigignore( SIGPIPE );
    
	struct evconnlistener *listener = NULL;
	struct event_base* single_event = NULL;
    struct evhttp* single_evhttp;
    struct sockaddr_in sin;
    
    
	sin.sin_family = AF_INET;
    sin.sin_port = htons(server->addr.port);
    
    event_set_mem_functions(fs_malloc, fs_realloc, fs_free);
    
    single_event = event_base_new();
    
    fs_assert(single_event != NULL);
    
    switch (server->server_type) {
        case t_fs_server_tcp:{
            listener = evconnlistener_new_bind(single_event,
                                               libevent_cb_listener,
                                               server,
                                               LEV_OPT_REUSEABLE|LEV_OPT_CLOSE_ON_FREE,
                                               -1,
                                               (struct sockaddr*)&sin,
                                               sizeof(sin));
            fs_assert(listener != NULL);
        }
            break;
        case t_fs_server_http:{
            single_evhttp = evhttp_new(single_event);
            fs_assert(single_evhttp != NULL);
            
            evhttp_set_gencb(single_evhttp, fs_server_http_request, server);
            evhttp_bind_socket_with_handle(single_evhttp, server->addr.addr, server->addr.port);
        }
            break;
        default:
            break;
    }
    
    
    server->event = single_event;
    
    if(server->_fn_on_start){
        server->_fn_on_start(server);
    }
    
	server->signal_event = evsignal_new(single_event, SIGINT, libevent_cb_signal, server);
    
    event_add(server->signal_event, NULL);
    
#ifdef __APPLE__
    pthread_setname_np(server->name);
#endif
    
    struct event* event = NULL;
    while (server->running) {
       
        event = NULL;
        do{
            event = fs_loop_queue_pop(server->event_loopque);
            if(event && event != (void*)1){
                event_add(event, NULL);
            }
        }while( event != NULL );
        server->need_pop_event = fs_true;
        
        event_base_loop(single_event, EVLOOP_ONCE);
    }
    
    return NULL;
}


struct fs_server*
fs_create_server(const char* server_name ){

    struct fs_server* ret = (struct fs_server*)( fs_malloc(sizeof(*ret)) );
    fs_zero(ret, sizeof(*ret));
    strncpy(ret->name, server_name, 64);
    ret->loopque = fs_create_loop_queue(LOOP_QUE_LEN);
    ret->event_loopque = fs_create_loop_queue(512);
    ret->need_pop_event = fs_true;
    hmap_create (&ret->node_map, HASHMAP_SIZE);
    
    return ret;
    
};

void
fs_server_start(struct fs_server* server, struct fs_node_addr* addr, enum fs_server_type type){
    
    memcpy(&server->addr, addr, sizeof(struct fs_node_addr));
    server->server_type = type;
    server->running = fs_true;
    
    pthread_create(&server->pthread_io_id,   NULL, fs_server_io_thread, server);
    
    
    if(server->create_work_thread){
        pthread_create(&server->pthread_work_id, NULL, fs_server_work_thread, server);
        
        pthread_mutex_init(&server->pthread_work_mutex, NULL);
        pthread_cond_init(&server->pthread_work_cond, NULL);
    
        
    }
        
    
}


void
__hahs_destroy_fn(void* data){
    struct fs_node* node = (struct fs_node*)data;
    fs_node_shudown(node);
    
}

void
fs_server_stop(struct fs_server* server, int32_t what){
    
    server->running = fs_false;
    
    struct fs_pack* pack = NULL;
    do{
        pack = (struct fs_pack*)fs_loop_queue_pop(server->loopque);
        if(pack){
            fs_free(pack);
        }
    }while (pack != NULL);
    
    hmap_destroy(server->node_map, __hahs_destroy_fn);
    
    event_base_free(server->event);
    
    pthread_cond_signal(&server->pthread_work_cond);
    
}


void
fs_server_set_name(struct fs_server* server, const char* name){
    strncpy(server->name, name, 64);
}

const char*
fs_server_get_name(struct fs_server* server){
    return server->name;
}

fs_bool
fs_server_connect_node(struct fs_server* server, struct fs_node* node, struct fs_node_addr* addr){
    
    int sock = socket(PF_INET, SOCK_STREAM, IPPROTO_TCP);
    struct sockaddr_in sin;
    sin.sin_family = AF_INET;
    sin.sin_port = htons(addr->port);
    sin.sin_addr.s_addr = inet_addr(addr->addr);
    int ret = connect(sock, (struct sockaddr*)&sin, sizeof(sin));
    
    if(ret != 0){
        return fs_false;
    }
    
    fs_id node_id = fs_server_next_connect_id(server);
    
    ret = fs_node_bind_event(node, node_id, sock, (struct sockaddr*)&sin, sizeof(sin), server, server->event);
    
    fs_server_add_node(server, node_id, node);
    
    
    return ret;
}


fs_bool
fs_server_on_recv_pack(struct fs_server* server, struct fs_pack* pack){
    
    fs_bool ret = fs_true;
    
    if(fs_loop_queue_push(server->loopque, pack)){
        fs_server_need_work(server);
    }else{
        while(!fs_loop_queue_push(server->loopque, pack)){
            fs_server_need_work(server);
            usleep(100);
        }
        fs_server_need_work(server);
    }
    
    return ret;
}


void
fs_server_set_handle_pack_fn(struct fs_server* server, fn_fs_pack_handle fn){
    server->_fn_pack_handle = fn;
}

void
fs_server_set_parsepack_fn(struct fs_server* server, fn_fs_pack_parse fn){
    server->_fn_pack_parse = fn;
}


void
fs_server_set_topack_fn(struct fs_server* server, fn_fs_pack_to_data fn){
    server->_fn_pack_to_data  = fn;
}

void
fs_server_set_on_server_start(struct fs_server* server, fn_fs_server_on_start fn){
    server->_fn_on_start = fn;
}

void
fs_server_set_node_connect(struct fs_server* server, fn_fs_node_connect fn){
    server->_fn_node_connect = fn;
}

void
fs_server_set_node_shudwon(struct fs_server* server, fn_fs_node_shudown fn){
    server->_fn_node_shudown = fn;
    
}

void
fs_server_set_script_id( struct fs_server* server, fs_script_id _id ){
    server->script_id = _id;
}

fs_script_id
fs_server_get_script_id( struct fs_server* server){
    return server->script_id;
}

enum fs_server_type
fs_server_get_type( struct fs_server* server){
    return server->server_type;
}

void
fs_server_add_event( struct fs_server* server, struct event* event){
    
    if(pthread_self() != server->pthread_io_id) {
        fs_loop_queue_push(server->event_loopque, event);
        event_base_loopbreak(server->event);
        pthread_kill(server->pthread_io_id, SIGINFO);
    }else{
        event_add(event, NULL);
    }
    
}


void
fs_server_need_work( struct fs_server* server){
    if(server->create_work_thread){
        pthread_cond_signal(&server->pthread_work_cond);
    }else{
        fs_server_handle_pack(server);
    }
}

struct fs_node*
fs_server_find_node_by_id(struct fs_server* server, fs_id id){
 
    char key[64];
    snprintf(key, 64, "%d", id);
    
    void* value = hmap_search(server->node_map, key);
    
    if(value){
        return (struct fs_node*)value;
    }
    
    return NULL;
}

void
fs_server_on_node_shudown(struct fs_server* server, fs_id node_id){
    if(server->_fn_node_shudown){
        server->_fn_node_shudown(server, node_id);
    }
    fs_server_rm_node(server, node_id);
}

fs_bool
fs_server_send_pack_node(struct fs_server* server, fs_id node_id, struct fs_pack* pack){
    
    struct fs_node* node = fs_server_find_node_by_id(server, node_id);
    if(node){
        return fs_server_send_pack_node_by_node(server, node, pack);
    }
    return fs_false;
}

fs_bool
fs_server_send_pack_node_by_node(struct fs_server* server, struct fs_node* node, struct fs_pack* pack){
    
    BYTE* data = NULL;
    size_t len = server->_fn_pack_to_data(server, pack, &data);
    if(node != NULL){
        fs_assert(len != 0);
        fs_node_send_data(node, data, len);
        fs_free(data);
        return fs_true;
    }
    
    return fs_false;
}

fs_bool
fs_server_send_data_node_by_node(struct fs_server* server, struct fs_node* node, void* data, size_t len){
    
    fs_assert(server != NULL);
    fs_assert(node != NULL);
    fs_assert(data != NULL);
    
    fs_node_send_data(node, (BYTE*)data, len);
    fs_free(data);
    return fs_true;
}

fs_bool
fs_server_close_node(struct fs_server* server, fs_id node_id){
    
    struct fs_node* node = fs_server_find_node_by_id(server, node_id);
    
    if(node){
        fs_node_close(node);
        return fs_true;
    }
    
    
    return fs_false;
}

fs_bool
fs_server_handle_pack(struct fs_server* server ){
    
    struct fs_pack* pack = fs_loop_queue_pop(server->loopque);
    if(pack == NULL){
        return fs_false;
    }
    
    if(server->_fn_pack_handle){
        return server->_fn_pack_handle(server, pack);
    }
    return fs_true;
}


size_t
fs_server_parse_pack(struct fs_server* server, const BYTE* data, ssize_t len, fs_id node_id, struct fs_pack** pack){
    size_t pack_len = 0;
    if(server->_fn_pack_parse){
        pack_len = server->_fn_pack_parse(server, data, len, node_id, pack);
    }
    return pack_len;
}


