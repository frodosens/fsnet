//
//  FSClientImpl.h
//  fsnet_client
//
//  Created by Vincent on 15/5/5.
//  Copyright (c) 2015年 Vincent. All rights reserved.
//

#ifndef __fsnet_client__FSClientImpl__
#define __fsnet_client__FSClientImpl__

#include <stdio.h>
#include "FSClientBase.h"

class FSClientImpl : public FSClientBase{

    
protected:
    
    virtual void on_connect_successfly();
    virtual void on_connect_faild();
    virtual void on_disconnect();
    virtual fs_bool on_dispatch_pack(FSPack*);
    
public:
    FSClientImpl(const char* hostname, unsigned short port);
    
public:
    
    void connect_server();
    void create_channel();
    void destroy_channel();
    void message_channel( FSChannel* , const FSMethod* , FSParams* );
    void message_return_channel();
    void reconnect_server();
    
protected:
    void on_create_channel(FSPack*);
    void on_message_channel(FSPack*);
    void on_message_return_channel(FSPack*);
    void on_destroy_channel(FSPack*);
    
};

#endif /* defined(__fsnet_client__FSClientImpl__) */
