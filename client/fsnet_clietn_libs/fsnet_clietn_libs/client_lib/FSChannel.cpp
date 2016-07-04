//
//  FSChannel.cpp
//  fsnet_client
//
//  Created by Vincent on 15/5/5.
//  Copyright (c) 2015年 Vincent. All rights reserved.
//

#include "FSChannel.h"
#include "FSMethod.h"
#include "FSParams.h"
#include "FSClientImpl.h"
#include "fs_malloc.h"

#include <string>


void FSChannel::bindMethods(){

}

void FSChannel::bindRPCMethod(const char* method_name, FSMethod::fn_rpc_method fn){
    
    FSMethod method;
    method.set_name(method_name);
    method.set_func(fn);
    m_local_method_map.insert(std::make_pair(method_name, method));
    
}
void FSChannel::setClient(FSClientImpl* client){
    this->client_impl = client;
}


void FSChannel::call_server_method( const FSMethod* method, FSParams* params){
    
    if(this->client_impl){
        this->client_impl->message_channel(this, method, params);
    }
    
    
}

void FSChannel::call_local_method( const FSMethod* method, FSParams* params){
    
    FSRPCMethodMap::iterator ite = m_local_method_map.find(method->name());
    
    if(ite != m_local_method_map.end()){
        FSMethod method = ite->second;
        method.call(this, params);
    }
    
}

const FSMethod* FSChannel::findMethod( const char* method_name ) const{
    
    if(m_local_method_map.find(method_name) == m_local_method_map.end()){
        return NULL;
    }else{
        return &m_local_method_map.find(method_name)->second;
    }
    
}

const std::string& FSChannel::uuid(){
    return _uuid;
}

