//
//  FSPack.cpp
//  fsnet_client
//
//  Created by Vincent on 15/5/5.
//  Copyright (c) 2015年 Vincent. All rights reserved.
//

#include "FSPack.h"
#include "fs_malloc.h"



FSPack::FSPack():_data_length(0), method_name(NULL){
    
}

FSPack::~FSPack(){
    if(method_name){
        fs_free(method_name);
    }
}

void FSPack::init( struct fs_input_stream* fis){
    
    size_t pos1 = fs_input_stream_get_pos(fis);
    
    _header.init_from_stream(fis, false);
    fs_stream_read_string(fis, &this->method_name);
    _params.init_from_stream(fis, false);
    size_t pos2 = fs_input_stream_get_pos(fis);
    
    _data_length = pos2 - pos1;
    
    
}

void FSPack::init( struct fs_output_stream* os ){
    
    _os = os;
}

size_t FSPack::length(){
    return _data_length + 5;
}
size_t FSPack::data_length(){
    return _data_length;
}

const FSParams& FSPack::params(){
    return _params;
}


fs_bool FSPack::read_from_stream( struct fs_input_stream* fis ){
   
    
    // 最少4字节头
    if(fs_input_stream_get_len(fis) >= 5 /* FLAG + LENGTH */){
        
        fs_byte flage = fs_stream_read_byte(fis);
        
        uint32_t len = fs_stream_read_uint32(fis);
        
        // 剩余长度
        if(fs_input_stream_get_len(fis) >= len){
            
            this->init(fis);
            
            
            return fs_true;
        }
        
        fs_unused(flage);
        
        
    }
    
    return fs_false;
    
}

void FSPack::write_to_stream( struct fs_output_stream* os ){
    
    fs_stream_write_byte(os, 'F');
    
    fs_stream_write_int32(os, (uint32_t)fs_output_stream_get_len(_os) + 5);
    
    fs_stream_write_data(os, (fs_byte*)fs_output_stream_get_dataptr(_os), fs_output_stream_get_len(_os));
    
    
}

uint32_t FSPack::gen_serial(){
    
    static uint32_t _serial = 0;
    
    return ++_serial;
    
}
