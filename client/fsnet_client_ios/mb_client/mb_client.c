//
//  NetworkSystem.cpp
//  f1_race_stars
//
//  Created by Vincent on 14/11/6.
//  Copyright (c) 2014年 Codemasters. All rights reserved.
//


#include "fs_stream.h"
#include "fs_malloc.h"
#include "mb_client.h"
#include "mb_entity.h"
#include "hash.h"

#include <netinet/in.h>
#include <arpa/inet.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>




struct mb_pack* mb_client_parse_pack(struct mb_client*);
void mb_client_dispatch_pack(struct mb_client*, struct mb_pack*);
fs_bool mb_client_connect2( struct mb_client* );
fs_bool mb_client_do_send(struct mb_client*);
fs_bool mb_client_do_recv(struct mb_client*);


struct mb_client{
    
    int socket;
    struct sockaddr_in addr;
    
    fd_set socket_write_event;
    fd_set socket_read_event;
    fd_set socket_error_event;
    
    struct fs_output_stream* recv_stream;
    struct fs_output_stream* send_stream;
    
    void* callback[eMBCallbackTotal];
    void* user_data[eMBClientTotal];
    
    fs_bool connecting;
    
    struct StrMap* entity_map;
    
    struct timeval timeout;
    
};

struct mb_client*
mb_client_create(){
    
    struct mb_client* client = fs_malloc(sizeof(*client));
    fs_zero(client, sizeof(client));
    client->timeout.tv_sec = 5;
    client->timeout.tv_usec = 0;
    client->socket = fs_false;
    client->socket = -1;
    client->send_stream = fs_create_output_stream_ext;
    client->recv_stream = fs_create_output_stream_ext;
    client->connecting = fs_false;
    client->entity_map = sm_new(0xff);
    return client;

}


fs_bool
mb_client_tick(struct mb_client* client){
    
    
    
    FD_ZERO(&client->socket_write_event);
    FD_SET(client->socket, &client->socket_write_event);
    FD_ZERO(&client->socket_read_event);
    FD_SET(client->socket, &client->socket_read_event);
    FD_ZERO(&client->socket_error_event);
    FD_SET(client->socket, &client->socket_error_event);
    
    int ret = select(client->socket + 1, &client->socket_read_event, &client->socket_write_event, &client->socket_error_event, &client->timeout);
    
    if(ret < 0){
        // error
        
    }else if(ret == 0){
        // timeout
        
    }else{
        
        if(FD_ISSET(client->socket, &client->socket_error_event)){
            
            mb_client_close(client);
            
            return fs_false;
        }
        
        int error = 0, error_len;
        getsockopt(client->socket, SOL_SOCKET, SO_ERROR, &error, (socklen_t *)&error_len);
        
        if(error != 0){
            mb_client_close(client);
            return fs_false;
        }
        
        // connected
        if (FD_ISSET(client->socket, &client->socket_write_event)) {
            
            
            // 正在连接中
            if(client->connecting){
                
                fn_mb_client_on_connected connect_fn = NULL;
                connect_fn = (fn_mb_client_on_connected)client->callback[eMBCallbackConnected];
                connect_fn(client);
                client->connecting = fs_false;
                
            }else{
            
                // try to send
                if(!mb_client_do_send(client)){
                    return fs_false;
                }
       
            }
        }
        
        // can be read
        if (FD_ISSET(client->socket, &client->socket_read_event)){
            
            
            if(!mb_client_do_recv(client)){
                return fs_false;
            }
            
        }
    }
    
    return fs_true;
}



fs_bool mb_client_do_send(struct mb_client* client){

    
    if(fs_output_stream_get_len(client->send_stream) > 0){
        
        // 如果发现有数据 要发送, 但是连接已经断开.
        if(!mb_client_is_connected(client)){
            if(!mb_client_reconnect(client)){
                // can't to connect
            }
        }
        
        
        void* data = (void*)fs_output_stream_get_dataptr(client->send_stream);
        size_t len = fs_output_stream_get_len(client->send_stream);
        ssize_t nsize = len;
        ssize_t nwrite = 0;
        ssize_t nwrited = 0;
        
        // 每次tick 只发一次
        nwrite = send(client->socket, (BYTE*)data + (len - nsize), nsize, 0);
        if(nwrite > 0){
            nwrited += nwrite;
        }
        if(nwrite < nsize){
            if(nwrite == -1){
                if(errno == EAGAIN)
                {
                    
                }else{
                    mb_client_close(client);
                    return fs_false;
                }
            }
        }
        
        fs_output_stream_sub(client->send_stream, nwrited, len - nwrited);
        
        return fs_true;
        
    }
    return fs_true;
    
}

fs_bool mb_client_do_recv(struct mb_client* client){
    
    // 每次tick只读一次. 每读一次解一次包
    BYTE buffer[1024];
    ssize_t len = recv(client->socket, buffer, 1024, 0);
    size_t buff_len = 0;
    struct mb_pack* pack = NULL;
    
    if(len > 0){
        
        fs_stream_write_data(client->recv_stream, buffer, len);
        
        while((pack = mb_client_parse_pack(client)) != NULL){
            
            mb_client_dispatch_pack(client, pack);
            
            buff_len = fs_output_stream_get_len(client->recv_stream);
            
            fs_output_stream_sub(client->recv_stream, mb_pack_len(pack), buff_len - (mb_pack_len(pack)));
            
            mb_pack_free(pack);
            
            pack = NULL;
            
        }
        
    }else{
        if(len == -1){
            if(errno == EAGAIN){
                
            }else{
                mb_client_close(client);
                return fs_false;
            }
        }else{
            mb_client_close(client);
            return fs_false;
        }
    }
    
    
    return fs_true;
    
}


fs_bool
mb_client_connect2( struct mb_client* client ){
    
    int fdflags = fcntl(client->socket, F_GETFL, 0);
    if(fcntl(client->socket, F_SETFL, fdflags | O_NONBLOCK) < 0){
        return fs_false;
    }
    
    fn_mb_client_on_connected connect_fn = NULL;
    connect_fn = (fn_mb_client_on_connected)client->callback[eMBCallbackConnected];
    
    fn_mb_client_on_diconnected diconnect_fn = NULL;
    diconnect_fn = (fn_mb_client_on_diconnected)client->callback[eMBCallbackDiConnected];
    
    if(connect(client->socket, (struct sockaddr *)&client->addr, sizeof(struct sockaddr_in)) == -1){
        
        if(errno == EISCONN){
            if(connect_fn != NULL){
                connect_fn(client);
            }
        }
        if(errno != EINPROGRESS && errno != EALREADY && errno != EWOULDBLOCK){
            mb_client_close(client);
        }
    }else{
        if(connect_fn != NULL){
            connect_fn(client);
        }
    }
    
    // 开始等待可写
    client->connecting = fs_true;
    
    return fs_true;
    
}

fs_bool
mb_client_connect(struct mb_client* client, const char* hostname, uint16_t port){

    
    if(mb_client_is_connected(client)){
        return fs_true;
    }
    
    client->socket = socket( AF_INET, SOCK_STREAM, IPPROTO_TCP );
    
    client->addr.sin_family = AF_INET;
    client->addr.sin_port = htons(port);
    client->addr.sin_addr.s_addr = inet_addr(hostname);
    fs_zero(client->addr.sin_zero, sizeof(client->addr.sin_zero));
    
    return mb_client_connect2(client);
    
}

fs_bool
mb_client_reconnect(struct mb_client* client){
    
    client->socket = socket( AF_INET, SOCK_STREAM, IPPROTO_TCP );
    
    fs_zero(client->addr.sin_zero, sizeof(client->addr.sin_zero));
    
    
    return mb_client_connect2(client);
}

fs_bool
mb_client_send(struct mb_client* client, BYTE* data, size_t len){
    
    if(client->send_stream != NULL){
    
        fs_stream_write_data(client->send_stream, data, len);
    
        return fs_true;
    }
    
    return fs_false;
    
}

fs_bool
mb_client_send_pack(struct mb_client* client, struct mb_pack* pack){
    
    struct fs_output_stream* stream = fs_create_output_stream_ext;
    
    mb_pack_write_to_stream(pack, stream);
    
    fs_bool ret = mb_client_send(client, (BYTE*)fs_output_stream_get_dataptr(stream), fs_output_stream_get_len(stream));
    
    fs_stream_free_output(stream);
    
    return ret;
    
}


fs_bool
mb_client_is_connected(struct mb_client* client){
    return client->socket != -1;
}


fs_bool
mb_client_close(struct mb_client* client){
    int ret = close(client->socket);
    client->socket = -1;
    
    fn_mb_client_on_diconnected fn = NULL;
    fn = (fn_mb_client_on_diconnected)client->callback[eMBCallbackDiConnected];
    if(fn != NULL){
        fn(client);
    }
    
    
    return ret == 0;
}


void
mb_client_free(struct mb_client* client){
    mb_client_close(client);
    fs_stream_free_output(client->recv_stream);
    client->recv_stream = NULL;
    fs_stream_free_output(client->send_stream);
    client->send_stream = NULL;
    fs_free(client);
}



void
mb_client_set_data( struct mb_client* client, MBClientData type, void* data){
    client->user_data[type] = data;
}

void*
mb_client_get_data( struct mb_client* client, MBClientData type ){
    return client->user_data[type];
}

void
mb_client_set_fn(struct mb_client* client, MBCallbackFn type, void* function){
    client->callback[type] = function;
}

void
mb_client_set_entity(struct mb_client* client, const char* id, struct mb_entity* entity){
	
    if(entity != NULL){
        
        sm_put(client->entity_map, id, entity);
        
    }else{
        
        mb_client_destroy_entity(client, id);
        
    }
	
}

struct mb_entity*
mb_client_get_entity(struct mb_client* client, const char* id){
	
    if(sm_exists(client->entity_map, id)){
        return sm_get(client->entity_map, id);
    }
    
    return NULL;
		
}


void _entity_enum_func(const char *key, void *value, const void *obj, void** out){

    
    const char* name = (const char*)obj;
    struct mb_entity* entity = (value);
    if(!entity) return;
    
    if( strcmp(mb_entity_get_name(entity), name) == 0){
        
        *out = entity;
        
        return;
    }
    
}

struct mb_entity*
mb_client_get_entity_with_name(struct mb_client* client, const char* name){
    
    unsigned char id[17] = {0};
    str2md5(name, strlen(name), id);
    
    struct mb_entity* out = NULL;
    
    sm_enum(client->entity_map, _entity_enum_func, id, (void**)&out);
    
    return out;
    
}

void mb_client_destroy_entity(struct mb_client* client, const char* id ){
  
  	if(sm_exists(client->entity_map, id)){
        struct mb_entity* entity = sm_get(client->entity_map, id);
      	if(entity != NULL){
            mb_entity_free(entity);
      	}
        sm_put(client->entity_map, id, NULL);
  	}
  
}

struct mb_pack*
mb_client_parse_pack(struct mb_client* client){
    
    struct mb_pack* ret = NULL;
    
    struct fs_input_stream* fis = fs_create_input_stream(
                                                         fs_output_stream_get_dataptr(client->recv_stream),
                                                         fs_output_stream_get_len(client->recv_stream));
    
    
    // 最少4字节头
    if(fs_input_stream_get_len(fis) > 4 /* 4 */){
        BYTE order = fs_stream_read_byte(fis);
        
        int32_t len = fs_stream_read_int32(fis);
        
        // 剩余长度
        if(fs_input_stream_get_len(fis) >= len){
            
            void* buff = fs_malloc(len);
            
            uint32_t serial = fs_stream_read_uint32(fis);
            
            int16_t cmd_index = fs_stream_read_int16(fis);
            
            int8_t version = fs_stream_read_byte(fis);
            
            int16_t make_sum = fs_stream_read_uint16(fis);
            
            int32_t data_len = fs_stream_read_uint32(fis);
            
            size_t buff_len = fs_stream_read_data(fis, buff, data_len);
            
            ret = mb_pack_create(cmd_index, buff, buff_len);
            
            fs_free(buff);
            
            
            fs_unused(serial);
            fs_unused(version);
            fs_unused(make_sum);
        }
        
        fs_unused(order);
        
        
    }
    
    fs_stream_free_input(fis);
    
    return ret;
    
}



void
mb_client_dispatch_pack(struct mb_client* client, struct mb_pack* pack){
    
    if(client->callback[eMBCallbackRecvPack] != NULL){
        fn_mb_client_on_recv_pack fn = NULL;
        fn = (fn_mb_client_on_recv_pack)client->callback[eMBCallbackRecvPack];
        if(fn != NULL){
            fn(client, pack);
        }else{
            // have not handle
        }
    }
    
}



