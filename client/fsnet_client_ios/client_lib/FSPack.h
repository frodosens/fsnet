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

public:
    enum EPackCmdIndex {
        ePackCmdConnect = 1,
        ePackCmdCreateChannel,
        ePackCmdDestroyChannel,
        ePackCmdMessageChannel,
        ePackCmdMessageReturn,
        ePackCmdReconnect,
        ePackCmdMax,
    };
    
private:
    uint32_t _length;
    uint32_t _data_length;
    void* _data;
    uint32_t _serial;
    EPackCmdIndex _cmd_index;
    int8_t _version;
    int16_t _make_sum;
    FSParams _params;
    struct fs_output_stream* _os;
protected:
    
    void init(uint32_t length, uint32_t serial, int16_t cmd_index, int8_t version, int16_t make_sum, int32_t data_len, void* data);

public:
    FSPack();
    ~FSPack();
    void init( EPackCmdIndex, struct fs_output_stream* );
    uint32_t length();
    uint32_t data_length();
    EPackCmdIndex cmd();
    void* data();
    const FSParams& params();
    fs_bool read_from_stream( struct fs_input_stream* );
    void write_to_stream( struct fs_output_stream* );
    
    
private:
    static uint32_t gen_serial();
    
};

#endif /* defined(__fsnet_client__FSPack__) */
