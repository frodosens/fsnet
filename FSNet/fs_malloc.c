//
//  fs_malloc.c
//  fsnet
//
//  Created by Vincent on 14-5-20.
//  Copyright (c) 2014年 Vincent. All rights reserved.
//

#include "fs_define.h"
#include <jemalloc.h>

void*
fs_malloc(size_t len){
    return malloc(len);
}


void
fs_free(void* ptr){
    free(ptr);
}

void*
fs_realloc(void* ptr, size_t len) {
    return realloc(ptr, len);
}


void* fs_calloc(size_t n, size_t size){
    return calloc(n, size);
}

void
fs_zero(void* data, size_t len){
    memset(data, 0, len);
}

void fs_assert_f(fs_bool cond){
    if(!cond){
        abort();
    }
}