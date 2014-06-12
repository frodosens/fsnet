//
//  fs_loop_queue.c
//  fsnet
//
//  Created by Vincent on 14-5-22.
//  Copyright (c) 2014年 Vincent. All rights reserved.
//

#include <stdio.h>
#include "fs_loop_queue.h"
#include "fs_malloc.h"


struct fs_loop_queue{
    
    void** que;
    size_t head;
    size_t tail;
    size_t max_len;
    
    unsigned long script_id;
};


#define _pos_add(x, n, max) x = (x + n) % max

struct fs_loop_queue*
fs_create_loop_queue(size_t len){
    struct fs_loop_queue* queue = (struct fs_loop_queue*)fs_malloc(sizeof(*queue));
    fs_zero(queue, sizeof(*queue));
    queue->max_len = len;
    queue->que = fs_malloc( sizeof( void* ) * len );
    fs_zero(queue->que, sizeof( void* ) * len);
    return queue;
}

fs_bool
fs_loop_queue_push( struct fs_loop_queue* que, void* item){

    if(fs_loop_queue_full(que)){
        return fs_false;
    }
    
    que->que[que->head] = item;
    _pos_add(que->head, 1, que->max_len);
    
    
    return fs_true;
}

void*
fs_loop_queue_pop( struct fs_loop_queue* que ){

    if(fs_loop_queue_empty(que)){
        return NULL;
    }
    
    
    void* item = que->que[que->tail];
    que->que[que->tail] = NULL;
    _pos_add(que->tail, 1, que->max_len);
    
    return item;
    
}

fs_bool
fs_loop_queue_full( struct fs_loop_queue* que ){

    return ((que->head + 1) % que->max_len) == que->tail;
}

fs_bool
fs_loop_queue_empty( struct fs_loop_queue* que ){
    
    return que->head == que->tail;
}


void
fs_loop_queue_free( struct fs_loop_queue* que){
    
    fs_free(que->que);
    fs_free(que);
    
}
