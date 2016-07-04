//
//  FSChannelFactory.h
//  fsnet_client
//
//  Created by Vincent on 15/5/5.
//  Copyright (c) 2015年 Vincent. All rights reserved.
//

#ifndef __fsnet_client__FSChannelFactory__
#define __fsnet_client__FSChannelFactory__

#include <stdio.h>
#include <map>

#include "FSObject.h"

class FSClientImpl;
class FSChannel;
class FSChannelFactory{

    typedef FSChannel*(*fn_channel_create)(void);
    
private:
    
    std::map<std::string, FSChannel*> channels;
    std::map<std::string, fn_channel_create> channels_factory_fn;
    
    
    static FSChannelFactory* s_instance;
    
    FSChannelFactory();
    
protected:
    
public:
    
    static FSChannelFactory* instance();
    
    FSChannel* createChannel(FSClientImpl*, const char* channelname, const char* channel_uuid);

    FSChannel* findChannel(const char* channel_uuid);
    
    void destroyChannel(const char* channel_uuid);
    
    void registCreateFunc(const char* channel_name, fn_channel_create fn);
};

#define REGIST_CHANNEL(_CHANNEL_NAME_) \
static FSChannel* __CHANNEL_FACTORY_create_##_CHANNEL_NAME_##_factory_fn(){ \
FSChannel* ret = new _CHANNEL_NAME_(); \
ret->bindMethods();\
return ret;\
} \

#define FACTORY_BIND_CHANNEL(_CHANNEL_NAME_)\
FSChannelFactory::instance()->registCreateFunc(#_CHANNEL_NAME_, _CHANNEL_NAME_::__CHANNEL_FACTORY_create_##_CHANNEL_NAME_##_factory_fn);


#endif /* defined(__fsnet_client__FSChannelFactory__) */
