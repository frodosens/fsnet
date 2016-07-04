//
//  FSClientImpl.cpp
//  fsnet_client
//
//  Created by Vincent on 15/5/5.
//  Copyright (c) 2015年 Vincent. All rights reserved.
//

#include "FSClientImpl.h"
#include "FSChannel.h"
#include "FSChannelFactory.h"

FSClientImpl::FSClientImpl(const char* hostname, unsigned short port):FSClientBase(hostname, port){
    
}
FSClientImpl::~FSClientImpl(){
    
}

void FSClientImpl::on_connect_successfly(){
    FSClientBase::on_connect_successfly();
    connect_server();
}

void FSClientImpl::on_connect_faild(){
    FSClientBase::on_connect_faild();
}

void FSClientImpl::on_disconnect(){
    FSClientBase::on_disconnect();
}


fs_bool FSClientImpl::on_dispatch_pack(FSPack* pack){
    
    
    
    for(int i = 0 ; i < pack->params().size() ; i++){
        
        struct fs_output_stream* os = fs_create_output_stream_ext;
        
        
        FSParamsHash header;
        FSParamsArray params;
        header.write_to_stream(os, false);
        fs_stream_write_c_string(os, "service_message");
        
        
        params.setUTF8("service", pack->params().get<const char*>(i));
        params.setUTF8("method", "init_to_service");
        
        params.push("1234567890");
        params.push(123456789);
        
        params.write_to_stream(os, false);
        
        
        FSPack npack;
        npack.init(os);
        send_pack(&npack);
        
        fs_stream_free_output(os);
        
        
    }
    
    
    return fs_true;
}

//_/_/  Client -> Server  _/_/_/_/_/_/

void FSClientImpl::connect_server(){
    
    struct fs_output_stream* os = fs_create_output_stream_ext;
    
    FSParamsHash header;
    FSParamsArray params;
    header.write_to_stream(os, false);
    fs_stream_write_c_string(os, "new_connect");
    params.write_to_stream(os, false);
    
    FSPack pack;
    pack.init(os);
    send_pack(&pack);
    
    fs_stream_free_output(os);
}
void FSClientImpl::create_channel(){

}
void FSClientImpl::destroy_channel(){

}
void FSClientImpl::message_channel( FSChannel* channel, const FSMethod* method, FSParams* params){
    
    struct fs_output_stream* os = fs_create_output_stream_ext;
    
    fs_stream_write_string(os, channel->uuid().c_str(), channel->uuid().length());
    fs_stream_write_string(os, method->name().c_str(), method->name().length());
    params->write_to_stream(os, false);
    fs_stream_write_byte(os, 0);
    
//    FSPack pack;
//    pack.init(FSPack::ePackCmdMessageChannel, os);

//    send_pack(&pack);
    
    fs_stream_free_output(os);
    
}
void FSClientImpl::message_return_channel(){
    
}
void FSClientImpl::reconnect_server(){
    
}

//_/_/  Server -> Client  _/_/_/_/_/_/
void FSClientImpl::on_create_channel(FSPack* pack){
    
    
}
void FSClientImpl::on_message_channel(FSPack* pack){
    
    
}
void FSClientImpl::on_message_return_channel(FSPack*){
    
}
void FSClientImpl::on_destroy_channel(FSPack* pack){
    
    
}