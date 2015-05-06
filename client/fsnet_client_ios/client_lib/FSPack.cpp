//
//  FSPack.cpp
//  fsnet_client
//
//  Created by Vincent on 15/5/5.
//  Copyright (c) 2015年 Vincent. All rights reserved.
//

#include "FSPack.h"
#include "fs_malloc.h"



FSPack::FSPack():_length(0),_serial(0),_cmd_index(ePackCmdMax),_data_length(0),_version(0),_make_sum(0),_data(NULL){
    
}

FSPack::~FSPack(){
    if(_data){
        fs_free(_data);
    }
}

void FSPack::init(uint32_t length,
                  uint32_t serial,
                  int16_t cmd_index,
                  int8_t version,
                  int16_t make_sum,
                  int32_t data_len,
                  void* data){
    
    
    _length = (length);
    _data_length = (data_len);
    _serial = (serial);
    _cmd_index = (EPackCmdIndex)(cmd_index);
    _version = (version);
    _make_sum = (make_sum);
    _data = data;
    
    
}

void FSPack::init( EPackCmdIndex cmd_index, struct fs_output_stream* os ){
    
    _cmd_index = cmd_index;
    _make_sum = 0;
    _os = os;
}

uint32_t FSPack::length(){
    return _length;
}
uint32_t FSPack::data_length(){
    return _data_length;
}

FSPack::EPackCmdIndex FSPack::cmd(){
    return _cmd_index;
}

void* FSPack::data(){
    return _data;
}

const FSParams& FSPack::params(){
    return _params;
}


fs_bool FSPack::read_from_stream( struct fs_input_stream* fis ){
   
    
    // 最少4字节头
    if(fs_input_stream_get_len(fis) >= 5 /* FLAG + LENGTH */){
        
        BYTE flage = fs_stream_read_byte(fis);
        
        uint32_t len = fs_stream_read_uint32(fis);
        
        // 剩余长度
        if(fs_input_stream_get_len(fis) >= len){
            
            char* buff = (char*)fs_malloc(len);
            
            uint32_t serial = fs_stream_read_uint32(fis);
            
            int16_t cmd_index = fs_stream_read_int16(fis);
            
            int8_t version = fs_stream_read_byte(fis);
            
            int16_t make_sum = fs_stream_read_uint16(fis);
            
            int32_t data_len = fs_stream_read_uint32(fis);
            
            fs_stream_read_data(fis, buff, data_len);
            
            this->init(len, serial, cmd_index, version, make_sum, data_len, buff);
            
            
            fs_unused(cmd_index);
            fs_unused(serial);
            fs_unused(version);
            fs_unused(make_sum);
            
            
            return fs_true;
        }
        
        fs_unused(flage);
        
        
    }
    
    return fs_false;
    
}

void FSPack::write_to_stream( struct fs_output_stream* os ){
    
    fs_stream_write_byte(os, 'F');
    
    size_t full_len_pos = fs_output_stream_get_len(os);
    
    fs_stream_write_uint32(os, 0);   // length
    fs_stream_write_uint32(os, gen_serial());   // serial
    fs_stream_write_int16(os, (uint16_t)cmd()); // cmd_index
    fs_stream_write_byte(os, _version);    // version
    fs_stream_write_int16(os, _make_sum);   // make_sum
    
    size_t data_len_pos = fs_output_stream_get_len(os);
    fs_stream_write_uint32(os, 0);   // data_len
    
    if(_os){
        fs_stream_write_data(os, (BYTE*)fs_output_stream_get_dataptr(_os), fs_output_stream_get_len(_os));
    }
    
    size_t data_end_pos = fs_output_stream_get_len(os);
    
    
    fs_output_stream_skip_to(os, full_len_pos);
    fs_stream_write_uint32(os, (uint32_t)(data_end_pos - full_len_pos) + 1);
    
    fs_output_stream_skip_to(os, data_len_pos);
    fs_stream_write_uint32(os, (uint32_t)(data_end_pos - data_len_pos));
    
    fs_output_stream_skip_to(os, data_end_pos);
    
    
}

uint32_t FSPack::gen_serial(){
    
    static uint32_t _serial = 0;
    
    return ++_serial;
    
}
