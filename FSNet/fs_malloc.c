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

#ifdef __APPLE__
#include <jemalloc.h>
#endif

void*
fs_malloc(size_t len){
    
#ifdef JEMALLOC_HAVE_ATTR
    
    return je_malloc(len);
#else
    
    return malloc(len);
    
#endif
}


void
fs_free(void* ptr){
    
#ifdef JEMALLOC_HAVE_ATTR
    je_free(ptr);
#else
    
    free(ptr);
#endif
    
}

void*
fs_realloc(void* ptr, size_t len) {
    
#ifdef JEMALLOC_HAVE_ATTR
    return je_realloc(ptr, len);
#else
    
    return realloc(ptr, len);
#endif
    
}


void* fs_calloc(size_t n, size_t size){
#ifdef JEMALLOC_HAVE_ATTR
    return je_calloc(n, size);
#else
    
    return calloc(n, size);
#endif
    
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