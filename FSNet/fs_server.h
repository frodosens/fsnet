//
//  fs_server.h
//  fsnet
//
//  Created by Vincent on 14-5-20.
//  Copyright (c) 2014年 Vincent. All rights reserved.
//



#ifndef FSNet_fs_server_h
#define FSNet_fs_server_h

#include "fs_node.h"
#include "fs_define.h"
#include "fs_struct.h"
#include "fs_malloc.h"
#include "fs_stream.h"
#include "fs_loop_queue.h"

struct fs_node;
struct fs_pack;
struct fs_server;
struct event;

enum fs_server_type{
    t_fs_server_tcp,
    t_fs_server_http
};

typedef fs_bool(*fn_fs_pack_handle)(struct fs_server*, struct fs_pack* );
typedef size_t (*fn_fs_pack_parse) (struct fs_server*, const BYTE*, ssize_t len, fs_id send_id, struct fs_pack** out_pack);
typedef size_t (*fn_fs_pack_to_data)( struct fs_server*, struct fs_pack* , BYTE** );
typedef void (*fn_fs_server_on_start)( struct fs_server* );
typedef void (*fn_fs_node_connect)( struct fs_server*, fs_id );
typedef void (*fn_fs_node_shudown)( struct fs_server*, fs_id );


struct  fs_server* fs_create_server(const char* name);
void    fs_server_start(struct fs_server*, struct fs_node_addr*, enum fs_server_type);
void    fs_server_stop(struct fs_server* , int32_t what);
void    fs_server_set_name(struct fs_server* , const char* name);
const char* fs_server_get_name(struct fs_server*);
void    fs_server_set_handle_pack_fn(struct fs_server*, fn_fs_pack_handle);
void    fs_server_set_parsepack_fn(struct fs_server*, fn_fs_pack_parse);
void    fs_server_set_topack_fn(struct fs_server*, fn_fs_pack_to_data);
void    fs_server_set_on_server_start(struct fs_server*, fn_fs_server_on_start);
void    fs_server_set_node_connect(struct fs_server*, fn_fs_node_connect);
void    fs_server_set_node_shudwon(struct fs_server*, fn_fs_node_shudown);
void    fs_server_clean_callback(struct fs_server*);
void         fs_server_set_script_id( struct fs_server* , fs_script_id _id );
fs_script_id fs_server_get_script_id( struct fs_server* );
enum fs_server_type fs_server_get_type( struct fs_server* );


void    fs_server_need_work( struct fs_server* );

struct fs_node* fs_server_find_node_by_id(struct fs_server* server, fs_id);
void    fs_server_on_node_shudown(struct fs_server*, fs_id node_id);
fs_bool fs_server_connect_node(struct fs_server*, struct fs_node*, struct fs_node_addr*);
fs_bool fs_server_send_pack_node(struct fs_server*, fs_id node_id, struct fs_pack*);
fs_bool fs_server_send_pack_node_by_node(struct fs_server*, struct fs_node*, struct fs_pack*);
fs_bool fs_server_send_data_node_by_node(struct fs_server*, struct fs_node*, void* data, size_t len);
fs_bool fs_server_close_node(struct fs_server*, fs_id node_id);
fs_bool fs_server_on_recv_pack(struct fs_server* , struct fs_pack*);
fs_bool fs_server_handle_pack(struct fs_server* );
size_t  fs_server_parse_pack(struct fs_server*, const BYTE* , ssize_t len, fs_id node_id, struct fs_pack**);



#endif
