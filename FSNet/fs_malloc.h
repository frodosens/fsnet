//
//  fs_malloc.h
//  fsnet
//
//  Created by Vincent on 14-5-20.
//  Copyright (c) 2014年 Vincent. All rights reserved.
//

#ifndef FSNet_fs_malloc_h
#define FSNet_fs_malloc_h

#include "fs_define.h"


void* fs_malloc(size_t);
void fs_free(void*);
void* fs_realloc(void*, size_t);
void fs_zero(void*, size_t);
void* fs_calloc(size_t, size_t);

void fs_assert_f(fs_bool cond);


#define fs_assert(cond, msg) \
if(!(cond)) {\
printf("abort [%s]!! %s:%d \n", msg, __FILE__, __LINE__); \
} \
fs_assert_f(cond)


#endif
