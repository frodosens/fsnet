//
//  mb_entity.c
//  GS55ClientLib
//
//  Created by Vincent on 14/11/19.
//  Copyright (c) 2014年 Vincent. All rights reserved.
//

#include "mb_pack.h"
#include "mb_client.h"
#include "hash.h"
#include "fs_malloc.h"
#include "mb_entity.h"
#include "mb_proto.h"
#include "md5.h"

struct StrMap* __global_entity_bind = NULL;

struct mb_entity{

    struct mb_client* client;
    char id[128];
    char routes[256];
    char info[1024];
    char name[128];
    
    size_t id_len;
    size_t routes_len;
    size_t info_len;
    size_t name_len;
    
    
    struct StrMap* method_map;
    
};


struct mb_entity*
mb_entity_create( struct mb_client* client, const char* id, size_t id_len, const char* routes, size_t r_len, const char* info, size_t info_len, const char* name, fn_mb_entity_bind_method fn){
    
    struct mb_entity* ret = fs_malloc(sizeof(*ret));
    fs_zero(ret, sizeof(*ret));
    ret->method_map = sm_new(512);
    ret->client = client;
    memcpy(ret->id, id, id_len);
    memcpy(ret->routes, routes, r_len);
    memcpy(ret->info, info, info_len);
    memcpy(ret->name, name, 16);
    
    ret->name_len = strlen(name);
    ret->id_len = id_len;
    ret->routes_len = r_len;
    ret->info_len = info_len;
    
    if(fn == NULL){
        fn = (fn_mb_entity_bind_method)sm_get(__global_entity_bind, ret->name);
    }
    
    if(fn != NULL){
        fn(ret);
    }
    
    return ret;
    
}

void mb_entity_bind_method( struct mb_entity* entity, const char* method_name, fn_mb_entity_method fn){
    
    sm_put(entity->method_map, method_name, fn);
    
}


const char*
mb_entity_get_id( struct mb_entity* entity ){
    return entity->id;
}

size_t
mb_entity_get_len( struct mb_entity* entity ){
    return entity->id_len;
}

const char*
mb_entity_get_name( struct mb_entity* entity ){
    return entity->name;
}

size_t
mb_entity_get_name_len( struct mb_entity* entity ){
    return entity->name_len;
}

void
mb_entity_free( struct mb_entity* entity ){
	
    if(entity->method_map != NULL){
        sm_delete(entity->method_map);
    }
	fs_free(entity);
	
}

fs_bool
mb_entity_call_method( struct mb_entity* entity,
                        const char* method_md5,
                        int16_t method_index,
                        const void* params,
                        size_t params_len ){
    
    fn_mb_entity_method fn = (fn_mb_entity_method)sm_get(entity->method_map, method_md5);
    if (fn == NULL) {
        fs_assert(fn != NULL, "fn is null");
    }
    
    fn((const char*)method_md5, method_index, entity->client, entity, params, params_len);
    
    return fs_true;
}








