//
//  ChannelSimple.cpp
//  fsnet_client
//
//  Created by Vincent on 15/5/5.
//  Copyright (c) 2015年 Vincent. All rights reserved.
//

#include "LoginChannel.h"
#include "FSClientImpl.h"

#include <sys/time.h>
#include <time.h>

void LoginChannel::bindMethods(){
    
    BIND_LOCAL_METHOD(LoginChannel, init);
    BIND_LOCAL_METHOD(LoginChannel, login_ret);
    
    BIND_REMOTE_METHOD(LoginChannel, login);
    
}


void LoginChannel::init( const FSParams* params ){
    
    params->debug_print();
    
    login("admin", "123456");
    
    
}

static long long t1 = 0;

void LoginChannel::login_ret( const FSParams* ){
    
    
    timeval tv;
    gettimeofday(&tv, NULL);
    long long t2 = (tv.tv_sec * 1000 * 1000 + tv.tv_usec) ;
    printf("ret = %lld \n", t2 - t1);
    
    
}

void LoginChannel::login(const char* user_name, const char* user_pwd){
    
    
    FSParamsArray params;
    params.push(user_name);
    params.push(user_pwd);
    
    
    call_server_method(findMethod("login"), &params);
    
    
    timeval tv;
    gettimeofday(&tv, NULL);
    client_impl->tick(0);
    
    t1 = tv.tv_sec * 1000 * 1000 + tv.tv_usec;
}
