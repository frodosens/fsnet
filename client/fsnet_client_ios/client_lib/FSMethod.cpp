//
//  FSMethod.cpp
//  fsnet_client
//
//  Created by Vincent on 15/5/5.
//  Copyright (c) 2015年 Vincent. All rights reserved.
//

#include "FSMethod.h"


const std::string& FSMethod::name() const{
    return method_name;
}
const std::string& FSMethod::md5() const{
    return method_md5;
}
const uint16_t& FSMethod::index() const{
    return method_index;
}
void FSMethod::set_name(std::string name){
    method_name = name;
}
void FSMethod::set_index(uint16_t index){
    method_index = index;
}
void FSMethod::set_func(fn_rpc_method fn){
    method_function = fn;
}
void FSMethod::call(FSObject* owner,const FSParams* params, fs_bool return_flag) const{
    if(owner && method_function){
        (owner->*method_function)(params);
    }
}