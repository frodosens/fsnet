//
//  FSClientBase.h
//  fsnet_client
//
//  Created by Vincent on 15/5/5.
//  Copyright (c) 2015 Vincent. All rights reserved.
//

#ifndef __fsnet_client__FSClientBase__
#define __fsnet_client__FSClientBase__

#include "FSObject.h"
#include "FSChannel.h"
#include "FSPack.h"


#include "fs_malloc.h"
#include "fs_stream.h"


#ifdef WIN32
#define CRS_WINDOWS 1
#endif

#ifdef CRS_WINDOWS
#include <WinSock2.h>
typedef int ssize_t;
#else
#include <arpa/inet.h>
#include <unistd.h>
#endif
#include <errno.h>
#include <fcntl.h>
#include <string>
#include <map>


class FSClientBase : public FSObject{

    
private:
    int socket;
    
    
    struct sockaddr_in addr;
    
    fd_set socket_write_event;
    fd_set socket_read_event;
    fd_set socket_error_event;
    
    struct fs_output_stream* recv_stream;
    struct fs_output_stream* send_stream;
    
    
    fs_bool connecting;
    struct timeval timeout;
    
    char hostname[16];
    unsigned short port;
    
public:
    
    FSClientBase(const char* host, unsigned short port);
    virtual ~FSClientBase();
    
public:
    
    fs_bool tick(float dt);
    fs_bool connect();
    fs_bool connected();
    fs_bool send_pack(FSPack*);
	fs_bool close();
    
protected:
    virtual void on_connect_successfly();
    virtual void on_connect_faild();
    virtual void on_disconnect();
    virtual fs_bool on_dispatch_pack(FSPack*);
    
    fs_bool on_send();
    fs_bool on_recv();
    fs_bool on_parse_pack(FSPack* in);
    
    
};



#endif /* defined(__fsnet_client__FSClientBase__) */
