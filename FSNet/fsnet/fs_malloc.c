//
//  fs_malloc.c
//  fsnet
//
//  Created by Vincent on 14-5-20.
//  Copyright (c) 2014年 Vincent. All rights reserved.
//

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "fs_define.h"
#include <jemalloc.h>

void*
fs_malloc(size_t len){
//    return ruby_xmalloc(len);
    return je_malloc(len);
//   return malloc(len);
}


void
fs_free(void* ptr){
//    ruby_xfree(ptr);
    je_free(ptr);
//    free(ptr);
}

void*
fs_realloc(void* ptr, size_t len) {
//    return ruby_xrealloc(ptr, len);
    return je_realloc(ptr, len);
//    return realloc(ptr, len);
}


void
fs_zero(void* data, size_t len){
    memset(data, 0, len);
}

void fs_assert(fs_bool cond){
    if(!cond){
        abort();
    }
}