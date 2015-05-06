//
//  mb_protobuf.h
//  GS55ClientLib
//
//  Created by Vincent on 14/11/18.
//  Copyright (c) 2014年 Vincent. All rights reserved.
//

#ifndef __GS55ClientLib__mb_protobuf__
#define __GS55ClientLib__mb_protobuf__



#ifdef __cplusplus
extern "C" {
#endif

#include <stdio.h>
#include "fs_define.h"


struct mb_proto;
struct mb_client;
struct mb_pack;

typedef void(*fn_mb_protobuf_cb)( struct mb_client*,  struct mb_proto*, const void* params, size_t params_len );
    

struct mb_proto* mb_proto_create();

void mb_proto_bind( struct mb_proto*, int16_t, fn_mb_protobuf_cb );

void mb_proto_handle( struct mb_proto*, struct mb_client*, struct mb_pack* );

    
#ifdef __cplusplus
}
#endif

#endif /* defined(__GS55ClientLib__mb_protobuf__) */
