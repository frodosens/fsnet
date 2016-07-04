//
//  FSChannelFactory.cpp
//  fsnet_client
//
//  Created by Vincent on 15/5/5.
//  Copyright (c) 2015年 Vincent. All rights reserved.
//

#include "FSChannelFactory.h"
#include "FSChannel.h"
#include <string>


FSChannelFactory* FSChannelFactory::s_instance = NULL;
FSChannelFactory* FSChannelFactory::instance(){
    
    if(s_instance == NULL){
        s_instance = new FSChannelFactory();
    }
    
    return s_instance;
}

FSChannelFactory::FSChannelFactory(){
    
    
}

void FSChannelFactory::registCreateFunc(const char* channel_name, fn_channel_create fn){
    
    std::string str_channel_name = channel_name;
    
    channels_factory_fn.insert(std::make_pair(str_channel_name, fn));
    
}

FSChannel* FSChannelFactory::createChannel(FSClientImpl* client,const char* channelname, const char* channel_uuid){
    
    if(channels_factory_fn.find(channelname) == channels_factory_fn.end()){
        return NULL;
    }else{
        FSChannel* channel = channels_factory_fn.find(channelname)->second();
        channel->setClient(client);
        channel->_uuid = channel_uuid;
        channels.insert(std::make_pair(channel_uuid, channel));
        return channel;
    }
    
}

FSChannel* FSChannelFactory::findChannel(const char* channel_uuid){
    
    if(channels.find(channel_uuid) == channels.end()){
        return NULL;
    }else{
        return channels.find(channel_uuid)->second;
    }
    return NULL;
    
}

void FSChannelFactory::destroyChannel(const char* channel_uuid){
    if(channels.find(channel_uuid) == channels.end()){
        return ;
    }else{
        FSChannel* channel = channels.find(channel_uuid)->second;
        delete channel;
        channels.erase(channel_uuid);
    }
}