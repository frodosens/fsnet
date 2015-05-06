//
//  mb_protobuf.c
//  GS55ClientLib
//
//  Created by Vincent on 14/11/18.
//  Copyright (c) 2014年 Vincent. All rights reserved.
//

#include "mb_proto.h"
#include "fs_malloc.h"
#include "mb_pack.h"

struct mb_proto {
    
    fn_mb_protobuf_cb method_fn[0xff];
    
};


struct mb_proto*
mb_proto_create(){
    
    struct mb_proto* ret = fs_malloc(sizeof(*ret));
    fs_zero(ret, sizeof(*ret));
    return ret;
    
}

void
mb_proto_bind( struct mb_proto* proto, int16_t method_index, fn_mb_protobuf_cb fn ){
    
    proto->method_fn[method_index] = fn;
    
}

void
mb_proto_handle( struct mb_proto* proto, struct mb_client* client, struct mb_pack* pack ){

    fn_mb_protobuf_cb fn = proto->method_fn[mb_pack_cmd_index(pack)];
    if(fn != NULL){
        fn(client, proto, mb_pack_get_params(pack), mb_pack_get_params_len(pack));
    }
    
}


