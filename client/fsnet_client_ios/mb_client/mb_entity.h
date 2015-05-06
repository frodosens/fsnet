//
//  entitlty.h
//  GS55ClientLib
//
//  Created by Vincent on 14/11/19.
//  Copyright (c) 2014年 Vincent. All rights reserved.
//

#ifndef __GS55ClientLib__entitlty__
#define __GS55ClientLib__entitlty__




#ifdef __cplusplus
extern "C" {
#endif
    
#include <stdio.h>
#include "fs_define.h"
#include "hash.h"
#include "md5.h"
    
    struct mb_entity;
    struct mb_client;
    struct mb_pack;
    
    extern StrMap* __global_entity_bind;
    
#define MB_ENTITY_REGIST( ENTITY_NAME ) \
__attribute__((constructor)) void mb_entity_init_##ENTITY_NAME##_() {\
if(__global_entity_bind == NULL){ \
__global_entity_bind = sm_new(1024); \
}\
unsigned char buff[17] = {};\
memset(buff, 0, 17);\
str2md5(#ENTITY_NAME, strlen(#ENTITY_NAME), buff); \
sm_put(__global_entity_bind, (const char*)buff, (void*)mb_entity_##ENTITY_NAME##_regist_method);\
\
}
    
#define MB_ENTITY_BEGIN_REGIST( ENTITY_NAME ) void mb_entity_##ENTITY_NAME##_regist_method( struct mb_entity* entity ){
#define MB_ENTITY_BIND_METHOD( ENTITY_NAME, METHOD_NAME ) { \
unsigned char buff[17] = {0}; \
memset(buff, 0, 17);\
str2md5(#METHOD_NAME, strlen(#METHOD_NAME), buff); \
mb_entity_bind_method( entity, (const char*)buff, _mb_entity_##ENTITY_NAME##_##METHOD_NAME ); \
}
    
#define MB_ENTITY_BIND_REMOTE_METHOD( ENTITY_NAME, METHOD_NAME  ) MB_ENTITY_BIND_METHOD( ENTITY_NAME, METHOD_NAME )
    
#define MB_ENTITY_END_REGIST( ENTITY_NAME ) } ;MB_ENTITY_REGIST( ENTITY_NAME )
    
    
#define MB_ENTITY_DEFINE_METHOD( ENTITY_NAME, METHOD_NAMR , P0, P1, P2, P3 ) \
void _mb_entity_##ENTITY_NAME##_##METHOD_NAMR( const char* md5, int index, P0, P1, P2, P3 )
    
    
    typedef void(*fn_mb_entity_method)( const char* md5, int index,  struct mb_client*,  struct mb_entity*, const void* params, size_t params_len );
    typedef void(*fn_mb_entity_bind_method)( struct mb_entity* );
    
    
    void mb_entity_free( struct mb_entity* );
    void mb_entity_bind_method( struct mb_entity* , const char*, fn_mb_entity_method );
    const char* mb_entity_get_id( struct mb_entity* );
    size_t mb_entity_get_len( struct mb_entity* );
    
    const char* mb_entity_get_name( struct mb_entity* );
    size_t mb_entity_get_name_len( struct mb_entity* );
    
    struct mb_entity* mb_entity_create( struct mb_client*,
                                       const char* ,
                                       size_t,
                                       const char* ,
                                       size_t ,
                                       const char*, size_t,
                                       const char* name,
                                       fn_mb_entity_bind_method);
    
    fs_bool mb_entity_call_method( struct mb_entity*,
                                  const char* method_md5,
                                  int16_t method_index,
                                  const void* params,
                                  size_t params_len );
    
    
    void str2md5( const char* input, unsigned long input_len, unsigned char* out );
    
    
    
    
#ifdef __cplusplus
}
#endif

#endif /* defined(__GS55ClientLib__entitlty__) */
