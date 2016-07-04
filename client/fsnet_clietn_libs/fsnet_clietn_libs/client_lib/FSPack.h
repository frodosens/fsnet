//
//  FSPack.h
//  fsnet_client
//
//  Created by Vincent on 15/5/5.
//  Copyright (c) 2015年 Vincent. All rights reserved.
//

#ifndef __fsnet_client__FSPack__
#define __fsnet_client__FSPack__

#include <stdio.h>

#include "fs_define.h"
#include "fs_stream.h"

#include "FSObject.h"
#include "FSParams.h"



class FSClientBase;
class FSPack : public FSObject{
    
private:
    size_t _data_length;
    FSParamsHash _header;
    char* method_name;
    FSParamsArray _params;
    struct fs_output_stream* _os;
protected:
    
    void init(struct fs_input_stream* );

public:
    FSPack();
    ~FSPack();
    size_t length();
    size_t data_length();
    const FSParams& params();
    fs_bool read_from_stream( struct fs_input_stream* );
    void write_to_stream( struct fs_output_stream* );
    void init(struct fs_output_stream* os);
    
    
private:
    static uint32_t gen_serial();
    
};

#endif /* defined(__fsnet_client__FSPack__) */
