//
//  FSChannel.h
//  fsnet_client
//
//  Created by Vincent on 15/5/5.
//  Copyright (c) 2015年 Vincent. All rights reserved.
//

#ifndef __fsnet_client__FSChannel__
#define __fsnet_client__FSChannel__

#include <map>
#include <stdio.h>

#include "FSObject.h"
#include "FSMethod.h"
#include "FSParams.h"

class FSClientImpl;
class FSChannelFactory;
class FSChannel : public FSObject{

    friend FSChannelFactory;
    
    typedef std::map< std::string, FSMethod > FSRPCMethodMap;
    
private:
    
    FSRPCMethodMap m_local_method_map;
    FSRPCMethodMap m_remote_method_map;
    std::string _uuid;
protected:
    FSClientImpl* client_impl;
    
    
protected:
    virtual void bindRPCMethod(const char* method_name, FSMethod::fn_rpc_method);
    void setClient(FSClientImpl*);
public:
    virtual void bindMethods();
    void call_server_method( const FSMethod*, FSParams* );
    void call_local_method( const FSMethod*, FSParams* );
    const FSMethod* findMethod( const char* method_name ) const;
    const std::string& uuid();
    
    
};


#define REMOTE_METHOD
#define LOCAL_METHOD

#define BIND_LOCAL_METHOD( _CLASS_, _FUNC_ )\
bindRPCMethod(#_FUNC_, (FSMethod::fn_rpc_method)&_CLASS_::_FUNC_);

#define BIND_REMOTE_METHOD( _CLASS_, _FUNC_ )\
bindRPCMethod(#_FUNC_, NULL);



#endif /* defined(__fsnet_client__FSChannel__) */
