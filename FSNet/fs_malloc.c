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
    
#ifdef JEMALLOC_HAVE_ATTR
    return je_malloc(len);
#else
    
#ifdef __APPLE__
    return malloc(len);
#else
    return ruby_xmalloc(len);
#endif
    
#endif
}


void
fs_free(void* ptr){
    
#ifdef JEMALLOC_HAVE_ATTR
    je_free(ptr);
#else
    
#ifdef __APPLE__
    free(ptr);
#else
    ruby_xfree(ptr);
#endif
    
#endif
    
}

void*
fs_realloc(void* ptr, size_t len) {
    
#ifdef JEMALLOC_HAVE_ATTR
    return je_realloc(ptr, len);
#else
    
#ifdef __APPLE__
    return realloc(ptr, len);
#else
    return ruby_xrealloc(ptr, len);
#endif
    
#endif
    
}


void* fs_calloc(size_t n, size_t size){
#ifdef JEMALLOC_HAVE_ATTR
    return je_calloc(n, size);
#else
    
#ifdef __APPLE__
    return calloc(n, size);
#else
    return ruby_xcalloc(n, size);
#endif
    
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