//
//  fs_loop_queue.h
//  fsnet
//
//  Created by Vincent on 14-5-22.
//  Copyright (c) 2014年 Vincent. All rights reserved.
//

#ifndef fsnet_fs_loop_queue_h
#define fsnet_fs_loop_queue_h

#include "fs_define.h"

struct fs_loop_queue;


struct fs_loop_queue* fs_create_loop_queue(size_t len);

fs_bool fs_loop_queue_push(struct fs_loop_queue*, void* item);
void*   fs_loop_queue_pop(struct fs_loop_queue*);

fs_bool fs_loop_queue_full(struct fs_loop_queue*);
fs_bool fs_loop_queue_empty(struct fs_loop_queue*);

void fs_loop_queue_free( struct fs_loop_queue* );

#endif
