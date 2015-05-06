//
//  mb_pack.h
//  GS55ClientLib
//
//  Created by Vincent on 14/11/18.
//  Copyright (c) 2014年 Vincent. All rights reserved.
//

#ifndef __GS55ClientLib__mb_pack__
#define __GS55ClientLib__mb_pack__



#ifdef __cplusplus
extern "C" {
#endif
    
#include "fs_define.h"

struct fs_output_stream;

    
struct mb_pack{
    
    int32_t serial;
    int8_t version;
    
    size_t len;
    int16_t cmd_index;
    void* params;
    size_t params_len;
        
};
    

    
    
struct mb_pack* mb_pack_create(int16_t cmd_index, void* params, size_t params_len);
void mb_pack_init( struct mb_pack*,  int16_t cmd_index, void* params, size_t params_len );
void mb_pack_free(struct mb_pack*);
uint16_t mb_pack_make_sum( struct mb_pack* );
    
    
size_t mb_pack_len( struct mb_pack* );
int16_t mb_pack_cmd_index( struct mb_pack* );
const void* mb_pack_get_params( struct mb_pack* );
size_t mb_pack_get_params_len( struct mb_pack* );
size_t mb_pack_write_to_stream( struct mb_pack*,  struct fs_output_stream* );
    
    

    

    
    
#ifdef __cplusplus
}
#endif
    
#endif /* defined(__GS55ClientLib__mb_pack__) */
