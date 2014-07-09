//
//  fs_stream.c
//  fsnet
//
//  Created by Vincent on 14-5-22.
//  Copyright (c) 2014年 Vincent. All rights reserved.
//
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "fs_define.h"
#include "fs_stream.h"
#include "fs_malloc.h"

#define BASE_STREAM\
    BYTE byte_order;    \
    BYTE* data; \


struct fs_input_stream{
    BASE_STREAM
    size_t pos;
    size_t len;
    fs_bool copy;
    fs_script_id script_id;
};
struct fs_output_stream{
    BASE_STREAM
    size_t pos;
    size_t buff_len;
    fs_bool weak_ref;
    fs_script_id script_id;
};


struct fs_output_stream*
fs_create_output_stream(size_t len){
    
    struct fs_output_stream* ret = (struct fs_output_stream*)fs_malloc(sizeof(*ret));
    fs_zero(ret, sizeof(*ret));
    ret->data = (BYTE*)fs_malloc(len);
    ret->pos = 0;
    ret->buff_len = len;
    ret->weak_ref = fs_false;
    
    return ret;
}


struct fs_output_stream*
fs_create_output_with_data( BYTE* data, size_t len){
    
    struct fs_output_stream* ret = (struct fs_output_stream*)fs_malloc(sizeof(*ret));
    fs_zero(ret, sizeof(*ret));
    ret->data = data;
    ret->pos = 0;
    ret->buff_len = len;
    ret->weak_ref = fs_true;
    return ret;
}

void
fs_stream_free_output( struct fs_output_stream* stream){
    
    if(!stream->weak_ref){
        fs_free(stream->data);
    }
    fs_free(stream);
    
}

void
fs_stream_write_byte( struct fs_output_stream* stream,  BYTE v){
    fs_stream_write_data(stream, (BYTE*)&v, sizeof(BYTE));
}

void
fs_stream_write_uint32( struct fs_output_stream* stream,  uint32_t v){
    fs_stream_write_data(stream, (BYTE*)&v, sizeof(uint32_t));
}

void
fs_stream_write_uint16( struct fs_output_stream* stream,  uint16_t v){
    fs_stream_write_data(stream, (BYTE*)&v, sizeof(uint16_t));
}

void
fs_stream_write_int32( struct fs_output_stream* stream,  int32_t v){
    fs_stream_write_data(stream, (BYTE*)&v, sizeof(int32_t));
}

void
fs_stream_write_int16( struct fs_output_stream* stream,  int16_t v){
    fs_stream_write_data(stream, (BYTE*)&v, sizeof(int16_t));
}

void
fs_stream_write_float( struct fs_output_stream* stream,  float v){
    fs_stream_write_data(stream, (BYTE*)&v, sizeof(float));
}

void
fs_stream_write_double( struct fs_output_stream* stream,  double v){
    fs_stream_write_data(stream, (BYTE*)&v, sizeof(double));
}

void
fs_stream_write_string( struct fs_output_stream* stream,  const char* data, size_t len ){
    fs_stream_write_uint16(stream, len);
    fs_stream_write_data(stream, (BYTE*)data, len);
}

void
fs_stream_write_c_string( struct fs_output_stream* stream,  const char* string){
    fs_stream_write_string(stream, string, strlen(string));
}

void
fs_stream_write_data( struct fs_output_stream* stream,  BYTE* data, size_t len ){
    
    if(stream->weak_ref){
        if(stream->pos + len > stream->buff_len){
            fs_assert(fs_true, "OutputStrem EOF");// 纯引用,出现溢出
        }
    }
    
    if(stream->buff_len == 0) stream->buff_len = 1;
    while(stream->pos + len > stream->buff_len){
        void* data = fs_realloc(stream->data, stream->buff_len << 1);
        stream->data = data;
        stream->buff_len = stream->buff_len << 1;
    }
    
    memcpy(stream->data + stream->pos, data, len);
    stream->pos += len;
}

void
fs_stream_write_long( struct fs_output_stream* stream, long v){
    fs_stream_write_data(stream, (BYTE*)&v, sizeof(long));
}
void
fs_stream_write_ulong( struct fs_output_stream* stream, unsigned long v){
    fs_stream_write_data(stream, (BYTE*)&v, sizeof(unsigned long));
}
void
fs_stream_write_int64( struct fs_output_stream* stream, int64_t v){
    uint32_t height = v >> 32;
    uint32_t low    = v & 0xffffffff;
    fs_stream_write_uint32(stream, height);
    fs_stream_write_uint32(stream, low);
}
void
fs_stream_write_uint64( struct fs_output_stream* stream, uint64_t v){
    fs_stream_write_int64(stream, v);
}

const BYTE*
fs_input_stream_get_data_ptr( struct fs_input_stream* stream){
    return stream->data;
}

const BYTE*
fs_output_stream_get_dataptr( struct fs_output_stream* stream){
    return stream->data;
}

size_t fs_output_stream_get_len( struct fs_output_stream* stream){
    return stream->pos;
}

fs_bool
fs_output_stream_skip_to( struct fs_output_stream* stream, size_t pos){
    if(pos >= stream->buff_len){
        return fs_false;
    }
    stream->pos = pos;
    return fs_true;
}

size_t
fs_output_stream_sub( struct fs_output_stream* stream, size_t start, size_t len ){
    memcpy(stream->data, stream->data + start, len);
    stream->pos = len;
    
    if(len < 0xff && stream->buff_len > 0xffff){
        stream->data = fs_realloc(stream->data, 0xff);
        stream->buff_len = 0xff;
    }
    
    return len;
}

struct fs_input_stream*
fs_create_input_stream(const BYTE* data, size_t len){
        
    struct fs_input_stream* ret = (struct fs_input_stream*)fs_malloc(sizeof(*ret));
    fs_zero(ret, sizeof(*ret));
    fs_input_stream_set_data(ret, data, len);
    return ret;

}

struct fs_input_stream*
fs_create_input_stream_with_copy(const BYTE* data, size_t len){
    
    struct fs_input_stream* ret = (struct fs_input_stream*)fs_malloc(sizeof(*ret));
    fs_zero(ret, sizeof(*ret));
    fs_input_stream_set_data_copy(ret, data, len);
    return ret;
}


void
fs_input_stream_set_data( struct fs_input_stream*  stream, const BYTE* data, size_t len){
    if(stream->data){
        if(stream->copy){
            fs_free(stream->data);
        }
    }
    stream->data = (BYTE*)data;
    stream->pos = 0;
    stream->len = len;
    stream->copy = fs_false;
}

void
fs_input_stream_set_data_copy( struct fs_input_stream* stream, const BYTE* data, size_t len){
    if(stream->data){
        if(stream->copy){
            fs_free(stream->data);
        }
    }
    
    stream->data = (BYTE*)fs_malloc(len);
    stream->pos = 0;
    stream->len = len;
    stream->copy = fs_true;
    memcpy(stream->data, data, len);
}

size_t
fs_input_stream_get_len( struct fs_input_stream* stream){
    return stream->len;
}

fs_bool
fs_input_stream_skip_to( struct fs_input_stream* stream , size_t pos){
    if(pos >= stream->len){
        return fs_false;
    }
    stream->pos = pos;
    return fs_true;
}

size_t
fs_input_stream_get_pos( struct fs_input_stream* stream){
    return stream->pos;
}

void
fs_input_stream_set_order( struct fs_input_stream* stream,  BYTE order){
    stream->byte_order = order;
}

void
fs_stream_free_input( struct fs_input_stream* stream){
    if(stream->copy){
        if (stream->data) {
            fs_free(stream->data);
        }
    }
    fs_free(stream);
}

#define _stream_read(TYPE, CONVER)\
TYPE v = 0; \
if(stream->pos + sizeof(TYPE) > stream->len){    \
return v;   \
}\
memcpy(&v, stream->data + stream->pos, sizeof(v));  \
stream->pos += sizeof(v); \
return v;

BYTE fs_stream_read_byte( struct fs_input_stream* stream){
    _stream_read(BYTE, htonl);
}
uint32_t fs_stream_read_uint32( struct fs_input_stream* stream){
    _stream_read(uint32_t, htonl);
}
uint16_t fs_stream_read_uint16( struct fs_input_stream* stream){
    _stream_read(uint16_t, htons);
}
int32_t fs_stream_read_int32( struct fs_input_stream* stream){
    _stream_read(int32_t, htonl);
}
int16_t fs_stream_read_int16( struct fs_input_stream* stream){
    _stream_read(int16_t, htons);
}
float fs_stream_read_float( struct fs_input_stream* stream){
    _stream_read(float, htonl);
}
double fs_stream_read_double( struct fs_input_stream* stream){
    _stream_read(double, );
}
size_t fs_stream_read_string( struct fs_input_stream* stream, char** out){
    uint16_t len = fs_stream_read_uint16(stream);
    if(stream->pos + len > stream->len){
        return 0;
    }
    *out = fs_malloc(len);
    memcpy(*out, stream->data + stream->pos, len);
    stream->pos += len;
    return len;
}
size_t fs_stream_read_data( struct fs_input_stream* stream, char* out, size_t len){
    if(stream->pos + len > stream->len){
        return 0;
    }
    memcpy(out, stream->data + stream->pos, len);
    stream->pos += len;
    return len;
}

long
fs_stream_read_long( struct fs_input_stream* stream){
    _stream_read(long, htonl);
}
unsigned long
fs_stream_read_ulong( struct fs_input_stream* stream){
    _stream_read(unsigned long, htonl);
}

int64_t
fs_stream_read_int64( struct fs_input_stream* stream){
    
	int64_t result = 0;
    
	uint32_t height = fs_stream_read_uint32(stream);
	uint32_t low    = fs_stream_read_uint32(stream);
    
	result = height;
	result = result << 32;
	result |= low;
	return result;
    
}
uint64_t
fs_stream_read_uint64( struct fs_input_stream* stream){
    return fs_stream_read_int64(stream);
}


void
fs_output_stream_set_script_id( struct fs_output_stream* stream, fs_script_id _id ){
    stream->script_id = _id;
}

fs_script_id
fs_output_stream_get_script_id( struct fs_output_stream* stream){
    return stream->script_id;
}

void
fs_input_stream_set_script_id( struct fs_input_stream* stream, fs_script_id _id ){
    stream->script_id = _id;
}


fs_script_id
fs_input_stream_get_script_id( struct fs_input_stream* stream){
    return stream->script_id;
}

