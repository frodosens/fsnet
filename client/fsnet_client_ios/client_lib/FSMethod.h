//
//  FSMethod.h
//  fsnet_client
//
//  Created by Vincent on 15/5/5.
//  Copyright (c) 2015年 Vincent. All rights reserved.
//

#ifndef __fsnet_client__FSMethod__
#define __fsnet_client__FSMethod__

#include <stdio.h>
#include <string>

#include "FSObject.h"
#include "fs_define.h"

class FSParams;
class FSChannel;
class FSMethod : public FSObject{
    
    friend FSChannel;
    
public:
    typedef void (FSObject::*fn_rpc_method)(const FSParams*);
    
private:
    std::string method_name;
    std::string method_md5;
    uint16_t method_index;
    fn_rpc_method method_function;
public:

    const std::string& name() const;
    const std::string& md5() const;
    const uint16_t& index() const;
    
    void set_name(std::string);
    void set_index(uint16_t);
    void set_func(fn_rpc_method);
    
    void call(FSObject* owner, const FSParams*, fs_bool return_flag=fs_false) const;
    
    
    
};

#endif /* defined(__fsnet_client__FSMethod__) */
