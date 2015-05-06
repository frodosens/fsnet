 //
//  mb_pack.c
//  GS55ClientLib
//
//  Created by Vincent on 14/11/18.
//  Copyright (c) 2014年 Vincent. All rights reserved.
//

#include "mb_pack.h"
#include "fs_malloc.h"
#include "fs_stream.h"


struct mb_pack*
mb_pack_create(int16_t cmd_index, void* params, size_t params_len){
    struct mb_pack* ret = fs_malloc(sizeof(*ret));
    fs_zero(ret, sizeof(*ret));
    mb_pack_init(ret, cmd_index, params, params_len);
    return ret;
}


void
mb_pack_init( struct mb_pack* pack,  int16_t cmd_index, void* params, size_t params_len ){
    
    pack->serial = 0;
    pack->version = 0;
    pack->len = 1 + 4 + 4 + 2 + 1 + 2 + 4 + params_len;
    pack->cmd_index = cmd_index;
    pack->params_len = params_len;
    pack->params = fs_malloc(params_len);
    memcpy(pack->params, params, params_len);
    
    
}

void mb_pack_free(struct mb_pack* pack){
    fs_free(pack->params);
    fs_free(pack);
}

size_t mb_pack_len( struct mb_pack* pack ){
    return pack->len;
}

int16_t mb_pack_cmd_index( struct mb_pack* pack ){
    return pack->cmd_index;
}

const void*
mb_pack_get_params( struct mb_pack* pack ){
    return pack->params;
}

size_t
mb_pack_get_params_len( struct mb_pack* pack ){
    return pack->params_len;
}

uint16_t
mb_pack_make_sum( struct mb_pack* pack ){
    return 0;
}
size_t
mb_pack_write_to_stream( struct mb_pack* pack, struct fs_output_stream* os ){
    
    size_t pre_len = fs_output_stream_get_len(os);
    fs_stream_write_byte(os, 0);
    fs_stream_write_int32(os, (int32_t)pack->len);
    fs_stream_write_int32(os, pack->serial);
    fs_stream_write_int16(os, pack->cmd_index);
    fs_stream_write_byte(os, pack->version);
    fs_stream_write_uint16(os, mb_pack_make_sum(pack));
    fs_stream_write_uint32(os, (uint32_t)pack->params_len);
    fs_stream_write_data(os, pack->params, pack->params_len);
    
    return fs_output_stream_get_len(os) - pre_len;
    
}
