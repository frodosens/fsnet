//
//  NetworkSystem.h
//  f1_race_stars
//
//  Created by Vincent on 14/11/6.
//  Copyright (c) 2014年 Codemasters. All rights reserved.
//

#ifndef __GS55ClientLib__mb_client__
#define __GS55ClientLib__mb_client__



#ifdef __cplusplus
extern "C" {
#endif

#include "fs_define.h"
#include "mb_pack.h"
    
typedef enum {
    eMBCallbackConnected = 0,
    eMBCallbackDiConnected,
    eMBCallbackRecvPack,
    eMBCallbackTotal,
} MBCallbackFn;
    
typedef enum {
    eMBClientData = 0,
    eMBClientProto,
    eMBClientTotal
} MBClientData;


struct fs_output_stream;
struct fs_input_stream;
struct mb_client;
struct mb_entity;

typedef void(*fn_mb_client_on_connected)(struct mb_client* );
typedef void(*fn_mb_client_on_diconnected)(struct mb_client* );
typedef void(*fn_mb_client_on_recv_pack)(struct mb_client*, struct mb_pack* );

struct mb_client* mb_client_create();
fs_bool mb_client_tick(struct mb_client* );
fs_bool mb_client_connect(struct mb_client* , const char* , uint16_t );
fs_bool mb_client_reconnect(struct mb_client*);
fs_bool mb_client_is_connected(struct mb_client*);
fs_bool mb_client_send(struct mb_client*, BYTE*, size_t);
fs_bool mb_client_send_pack(struct mb_client* , struct mb_pack* );
fs_bool mb_client_close(struct mb_client*);
void mb_client_free(struct mb_client*);
void mb_client_set_data( struct mb_client*, MBClientData, void*);
void* mb_client_get_data( struct mb_client*, MBClientData);
void mb_client_set_fn(struct mb_client*,  MBCallbackFn, void*);
void mb_client_set_entity(struct mb_client*, const char*, struct mb_entity*);
struct mb_entity* mb_client_get_entity(struct mb_client*, const char*);
struct mb_entity* mb_client_get_entity_with_name(struct mb_client*, const char* name);
void mb_client_destroy_entity(struct mb_client*, const char* );

    
#ifdef __cplusplus
}
#endif
    

#endif /* defined(__f1_race_stars__NetworkSystem__) */
