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
    
    switch (pack->cmd()) {
        case FSPack::ePackCmdConnect:
        case FSPack::ePackCmdReconnect:
            break;
        case FSPack::ePackCmdCreateChannel:
            on_create_channel(pack);break;
        case FSPack::ePackCmdDestroyChannel:
            on_destroy_channel(pack);break;
        case FSPack::ePackCmdMessageChannel:
            on_message_channel(pack);break;
        case FSPack::ePackCmdMessageReturn:
            on_message_return_channel(pack);break;
        default:
            break;
    }
    
    
    return fs_true;
}

//_/_/  Client -> Server  _/_/_/_/_/_/

void FSClientImpl::connect_server(){
    struct fs_output_stream* os = fs_create_output_stream_ext;
    
    fs_stream_write_string(os, "123", 4);
    FSPack pack;
    pack.init(FSPack::ePackCmdConnect, os);
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
    
    FSPack pack;
    pack.init(FSPack::ePackCmdMessageChannel, os);

    send_pack(&pack);
    
    fs_stream_free_output(os);
    
}
void FSClientImpl::message_return_channel(){
    
}
void FSClientImpl::reconnect_server(){
    
}

//_/_/  Server -> Client  _/_/_/_/_/_/
void FSClientImpl::on_create_channel(FSPack* pack){
    
    struct fs_input_stream* fis = fs_create_input_stream((unsigned char*)pack->data(), pack->length());
    char* channel_uuid;
    fs_stream_read_string(fis, &channel_uuid);
    char* channel_class;
    fs_stream_read_string(fis, &channel_class);
    char* remote_channel_class;
    fs_stream_read_string(fis, &remote_channel_class);
    FSParams params;
    params.init_from_stream(fis, false);
    
    fs_assert(FSChannelFactory::instance()->createChannel(this, channel_class, channel_uuid) != NULL, "create channel faild");
    
    
    fs_free(channel_uuid);
    fs_free(channel_class);
    fs_free(remote_channel_class);
    
    fs_stream_free_input(fis);
    
}
void FSClientImpl::on_message_channel(FSPack* pack){
    
    struct fs_input_stream* fis = fs_create_input_stream((unsigned char*)pack->data(), pack->length());
    
    char* channel_uuid;
    fs_stream_read_string(fis, &channel_uuid);
    char* channel_method_name;
    fs_stream_read_string(fis, &channel_method_name);
    FSParamsArray params;
    params.init_from_stream(fis, false);
    BYTE return_flag = fs_stream_read_byte(fis);
    
    FSChannel* channel = FSChannelFactory::instance()->findChannel(channel_uuid);
    const FSMethod* method = channel->findMethod(channel_method_name);
    if(method){
        method->call(channel, &params, return_flag);
    }
    
    
    fs_free(channel_uuid);
    fs_free(channel_method_name);
    fs_stream_free_input(fis);
    
    
}
void FSClientImpl::on_message_return_channel(FSPack*){
    
}
void FSClientImpl::on_destroy_channel(FSPack* pack){
    
    struct fs_input_stream* fis = fs_create_input_stream((unsigned char*)pack->data(), pack->length());
    char* channel_uuid;
    fs_stream_read_string(fis, &channel_uuid);
    
    FSChannelFactory::instance()->destroyChannel(channel_uuid);
    
    fs_free(channel_uuid);
    fs_stream_free_input(fis);
    
}