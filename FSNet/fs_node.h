//
//  fs_node.h
//  fsnet
//
//  Created by Vincent on 14-5-20.
//  Copyright (c) 2014年 Vincent. All rights reserved.
//

#ifndef FSNet_fs_node_h
#define FSNet_fs_node_h


#include "fs_define.h"

struct  fs_node;
struct  fs_server;
struct  fs_node_addr;
struct  event_base;
struct  fs_pack;

struct  fs_node* fs_create_node(struct fs_server*);
fs_bool fs_node_bind_event( struct fs_node* , fs_id , int , struct sockaddr*, socklen_t, struct fs_server*, struct event_base* );
void    fs_node_send_data(struct fs_node* node, BYTE* data, size_t len);
void    fs_node_shudown(struct fs_node* node);
void    fs_node_close_socket(struct fs_node* node);
void    fs_node_close(struct fs_node* node);
fs_id   fs_node_get_id(struct fs_node* node_id);
fs_bool fs_node_is_active( struct fs_node* );
fs_bool fs_node_is_from_connect( struct fs_node* );
fs_bool fs_node_is_from_listener( struct fs_node* );
fs_bool fs_node_is_closed( struct fs_node* );

void         fs_node_set_script_id( struct fs_node* , fs_script_id _id );
fs_script_id fs_node_get_script_id( struct fs_node* );


#endif
