//
//  fs_stream.h
//  fsnet
//
//  Created by Vincent on 14-5-22.
//  Copyright (c) 2014年 Vincent. All rights reserved.
//

#ifndef fsnet_fs_stream_h
#define fsnet_fs_stream_h


#ifdef __cplusplus
extern "C" {
#endif

#include <stdlib.h>
#include <stdio.h>
#include "fs_define.h"
    
#define OUTPUT_STRAEM_DEFAULT_MAX 64

struct fs_stream;
struct fs_input_stream;
struct fs_output_stream;


struct fs_output_stream* fs_create_output_stream(size_t len);
struct fs_output_stream* fs_create_output_with_data( BYTE*, size_t len);
#define fs_create_output_stream_ext fs_create_output_stream(OUTPUT_STRAEM_DEFAULT_MAX)
void fs_stream_free_output( struct fs_output_stream* );
void fs_stream_write_byte( struct fs_output_stream*,  BYTE);
void fs_stream_write_uint32( struct fs_output_stream*,  uint32_t);
void fs_stream_write_uint16( struct fs_output_stream*,  uint16_t);
void fs_stream_write_int32( struct fs_output_stream*,  int32_t);
void fs_stream_write_int16( struct fs_output_stream*,  int16_t);
void fs_stream_write_float( struct fs_output_stream*,  float );
void fs_stream_write_double( struct fs_output_stream*,  double );
void fs_stream_write_string( struct fs_output_stream*,  const char*, size_t len );
void fs_stream_write_c_string( struct fs_output_stream*,  const char* );
void fs_stream_write_data( struct fs_output_stream*,  BYTE*, size_t len );
void fs_stream_write_long( struct fs_output_stream*, long );
void fs_stream_write_ulong( struct fs_output_stream*, unsigned long );
void fs_stream_write_int64( struct fs_output_stream*, int64_t );
void fs_stream_write_uint64( struct fs_output_stream*, uint64_t );





struct fs_input_stream* fs_create_input_stream(const BYTE* data, size_t len);
struct fs_input_stream* fs_create_input_stream_with_copy(const BYTE* data, size_t len);

void fs_input_stream_set_data( struct fs_input_stream*, const BYTE* data, size_t len);
void fs_input_stream_set_data_copy( struct fs_input_stream*, const BYTE* data, size_t len);
size_t fs_input_stream_get_len( struct fs_input_stream* stream);
fs_bool fs_input_stream_skip_to( struct fs_input_stream* stream , size_t);
size_t fs_input_stream_get_pos( struct fs_input_stream* stream);
void fs_input_stream_set_order( struct fs_input_stream*,  BYTE order);
void fs_stream_free_input( struct fs_input_stream* );
BYTE fs_stream_read_byte( struct fs_input_stream* );
uint32_t fs_stream_read_uint32( struct fs_input_stream* );
uint16_t fs_stream_read_uint16( struct fs_input_stream* );
int32_t fs_stream_read_int32( struct fs_input_stream* );
int16_t fs_stream_read_int16( struct fs_input_stream* );
float fs_stream_read_float( struct fs_input_stream* );
double fs_stream_read_double( struct fs_input_stream* );
size_t fs_stream_read_string( struct fs_input_stream* , char**);
size_t fs_stream_read_data( struct fs_input_stream* , char*, size_t);
long fs_stream_read_long( struct fs_input_stream* );
unsigned long fs_stream_read_ulong( struct fs_input_stream* );
int64_t fs_stream_read_int64( struct fs_input_stream* );
uint64_t fs_stream_read_uint64( struct fs_input_stream* );

const BYTE* fs_input_stream_get_data_ptr( struct fs_input_stream* );
const BYTE* fs_output_stream_get_dataptr( struct fs_output_stream* );
size_t fs_output_stream_get_len( struct fs_output_stream* );
fs_bool fs_output_stream_skip_to( struct fs_output_stream* , size_t);

size_t fs_output_stream_sub( struct fs_output_stream*, size_t start, size_t len );
    
    
    
    
    
#ifdef __cplusplus
    }
#endif
    
#endif
