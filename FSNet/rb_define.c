//
//  rb_define_fs_server.c
//  fsnet
//
//  Created by Vincent on 14-5-23.
//  Copyright (c) 2014年 Vincent. All rights reserved.
//

#include <ruby.h>
#include <evhttp.h>
#include "rb_define.h"
#include "fs_server.h"
#include "fs_node.h"


static uint32_t pack_head_len = 5;

VALUE rb_cFSNet ;
VALUE rb_cServer ;
VALUE rb_cTimer ;
VALUE rb_cPack  ;
VALUE rb_cInputStream;
VALUE rb_cOutputStream;
VALUE rb_cNode;
VALUE rb_cHTTPRequest;
VALUE rb_cHTTPResponse;

void rb_define_fs_net();
void rb_define_fs_server();
void rb_define_fs_node();
void rb_define_fs_pack();
void rb_define_fs_stream();
void rb_define_fs_http();


struct fs_pack{
    
    fs_id           node_id;
    fs_pack_type    pack_type;
    void*           data;
    size_t          len;
    struct fs_output_stream* output_stream;
    struct fs_input_stream* input_stream;
    fs_script_id    script_id;
    
};

enum fs_sys_pack_type{
    
    fs_sys_pack_type_tick = -4,
    fs_sys_pack_type_start = -3,
    fs_sys_pack_type_connect = -2,
    fs_sys_pack_type_diconnect = -1
    
};

extern VALUE rb_cServer ;
extern VALUE rb_cPack  ;
extern VALUE rb_cInputStream;
extern VALUE rb_cOutputStream;
extern VALUE rb_cNode;
extern VALUE rb_cHTTPRequest;
extern VALUE rb_cHTTPResponse;



struct fs_invoke_call_function*
fs_create_invoke_call(VALUE (*func)(VALUE), int argc, VALUE* argv){
    
    VALUE *copy_argv = fs_malloc(argc * sizeof(VALUE));
    memcpy(copy_argv, argv, argc * sizeof(VALUE));
    
    struct fs_invoke_call_function* ret = (struct fs_invoke_call_function*)fs_malloc(sizeof(*ret));
    fs_zero(ret, sizeof(*ret));
    ret->func = func;
    ret->argc = argc;
    ret->argv = copy_argv;
    return ret;
}

void
fs_pack_free(struct fs_pack* pack){
    if(pack->data){
        fs_free(pack->data);
    }
    if(pack->input_stream){
        fs_stream_free_input(pack->input_stream);
    }
    fs_free(pack);
}

struct fs_pack*
fs_create_empty_pack(){
    struct fs_pack*  ret = (struct fs_pack*)fs_malloc(sizeof(*ret));
    fs_zero(ret, sizeof(*ret));
    return ret;
};
struct fs_pack*
fs_create_server_start(struct fs_server* server){
    struct fs_pack*  ret = fs_create_empty_pack();
    ret->pack_type = fs_sys_pack_type_start;
    ret->node_id = 0;
    return ret;
};
struct fs_pack*
fs_create_connect_pack(fs_id node_id){
    struct fs_pack*  ret = fs_create_empty_pack();
    ret->pack_type = fs_sys_pack_type_connect;
    ret->node_id = node_id;
    return ret;
};

struct fs_pack*
fs_create_diconnect_pack(fs_id node_id){
    struct fs_pack*  ret = fs_create_empty_pack();
    ret->pack_type = fs_sys_pack_type_diconnect;
    ret->node_id = node_id;
    return ret;
};
struct fs_pack*
fs_create_time_tick_pack(struct fs_server* server){
    
    struct fs_pack*  ret = fs_create_empty_pack();
    ret->pack_type = fs_sys_pack_type_tick;
    ret->node_id = 0;
    return ret;
    
}

size_t
fs_ruby_tcp_parse_pack(struct fs_server* server, const BYTE* data, ssize_t len, fs_id node_id,  struct fs_pack** pack){
    
    
#if 0
    if(len <= 0) return 0;
    *pack = fs_create_empty_pack();
    fs_zero(*pack, sizeof(**pack));
    (*pack)->data = fs_malloc(len);
    (*pack)->data_stream = fs_create_input_stream((*pack)->data, len);
    (*pack)->len = len;
    (*pack)->node_id = node_id;
    memcpy((*pack)->data, data, len);
    return len;
#endif
    
    
    if(len < pack_head_len) {
        return 0;
    }
    
//    BYTE order           = data[0];
    BYTE pack_len1       = data[1];
    BYTE pack_len2       = data[2];
    BYTE pack_len3       = data[3];
    BYTE pack_len4       = data[4];
    uint32_t pack_len    = (pack_len4 << 24 | pack_len3 << 16 | pack_len2 << 8 | pack_len1);
    
    if(len >= pack_len && pack_len >= pack_head_len){
        
        *pack = fs_create_empty_pack();
        struct fs_pack* ret_pack = *pack;
        fs_zero(ret_pack, sizeof(*ret_pack));
        ret_pack->node_id = node_id;
        ret_pack->data = fs_malloc(pack_len - pack_head_len/* 减去5个字节头 */);
        ret_pack->input_stream = fs_create_input_stream(ret_pack->data, len);
        ret_pack->len = pack_len - pack_head_len;
        
        
        memcpy(ret_pack->data, data + pack_head_len, pack_len - pack_head_len);
        
        
        return pack_len;
    }
    
    return 0;
    
}


size_t
fs_ruby_tcp_parse_pack_with_mb(struct fs_server* server, const BYTE* data, ssize_t len, fs_id node_id,  struct fs_pack** pack){
    
    const static uint32_t mb_pack_head_len = 4;
    
    if(len < mb_pack_head_len) {
        return 0;
    }
    BYTE pack_len1       = data[0];
    BYTE pack_len2       = data[1];
    BYTE pack_len3       = data[2];
    BYTE pack_len4       = data[3];
    uint32_t pack_len    = (pack_len4 << 24 | pack_len3 << 16 | pack_len2 << 8 | pack_len1);
    
    // 必须有一个rpc_index
    if(pack_len >= 2){
        
        *pack = fs_create_empty_pack();
        struct fs_pack* ret_pack = *pack;
        fs_zero(ret_pack, sizeof(*ret_pack));
        ret_pack->node_id = node_id;
        ret_pack->data = fs_malloc(pack_len);
        ret_pack->input_stream = fs_create_input_stream(ret_pack->data, len);
        ret_pack->len = pack_len;
        
        memcpy(ret_pack->data, data + mb_pack_head_len, pack_len);
        
        
        return mb_pack_head_len + pack_len;
        
        
    }
    
    return 0;
    
    
}


size_t
fs_ruby_http_parse_pack(struct fs_server* server, const BYTE* data, ssize_t len, fs_id node_id,  struct fs_pack** pack){
    
    if(len <= 0) return 0;
    *pack = fs_create_empty_pack();
    fs_zero(*pack, sizeof(**pack));
    (*pack)->data = fs_malloc(len);
    (*pack)->input_stream = fs_create_input_stream((*pack)->data, len);
    (*pack)->len = len;
    (*pack)->node_id = node_id;
    memcpy((*pack)->data, data, len);
    return len;
    
}

size_t
fs_ruby_parse_pack (struct fs_server* server, const BYTE* data, ssize_t len, fs_id node_id,  struct fs_pack** pack){
    
    switch (fs_server_get_type(server)) {
        case t_fs_server_tcp:
            return fs_ruby_tcp_parse_pack(server, data, len, node_id, pack);
            break;
        case t_fs_server_http:
            return fs_ruby_http_parse_pack(server, data, len, node_id, pack);
            break;
        default:
            break;
    }
    return 0;
}


size_t
fs_ruby_pack_to_data( struct fs_server* server, struct fs_pack* pack, BYTE** out){
    
    
    
    BYTE* data = NULL;
    size_t len = 0;
    
    VALUE write_data = rb_funcall(pack->script_id, rb_intern("write_data"), 0);
    
    if(write_data != Qnil){
        struct fs_output_stream* os = NULL;
        Data_Get_Struct(write_data, struct fs_output_stream, os);
        
        data = (BYTE*)fs_output_stream_get_dataptr(os);
        len = fs_output_stream_get_len(os);
        
    }
    
    VALUE read_data = rb_funcall(pack->script_id, rb_intern("read_data"), 0);
    if(read_data != Qnil){
        struct fs_input_stream* is = NULL;
        Data_Get_Struct(read_data, struct fs_input_stream, is);
        
        data = (BYTE*)fs_input_stream_get_data_ptr(is);
        len = fs_input_stream_get_len(is);
        
    }
    
    
    if (pack->input_stream != NULL) {
        data = (BYTE*)fs_input_stream_get_data_ptr(pack->input_stream);
        len = fs_input_stream_get_len(pack->input_stream);
    }
    
    
    //VALUE c_server = fs_server_get_script_id(server);
    //VALUE byte_order = rb_funcall(c_server, rb_intern("byte_order"), 0);
    
    struct fs_output_stream* fos = fs_create_output_stream_ext;
    fs_stream_write_byte(fos, 0);
    fs_stream_write_int32(fos, (int32_t)len + pack_head_len);
    fs_stream_write_data(fos, data, len);
    
    size_t ret_len = fs_output_stream_get_len(fos);
    
    *out = fs_malloc(ret_len);
    memcpy(*out, fs_output_stream_get_dataptr(fos), ret_len);
    
    fs_stream_free_output(fos);
    
    if(len == 0){
        rb_raise(rb_eRuntimeError, "try send data len for 0");
    }
    
    return ret_len;
}




size_t
fs_ruby_pack_to_data_with_mb( struct fs_server* server, struct fs_pack* pack, BYTE** out){
    
    
    const static uint32_t mb_pack_head_len = 4;
    
    BYTE* data = NULL;
    size_t len = 0;
    VALUE write_data = rb_funcall(pack->script_id, rb_intern("write_data"), 0);
    
    if(write_data != Qnil){
        struct fs_output_stream* os = NULL;
        Data_Get_Struct(write_data, struct fs_output_stream, os);
        
        data = (BYTE*)fs_output_stream_get_dataptr(os);
        len = fs_output_stream_get_len(os);
        
    }
    
    
    VALUE read_data = rb_funcall(pack->script_id, rb_intern("read_data"), 0);
    if(read_data != Qnil){
        struct fs_input_stream* is = NULL;
        Data_Get_Struct(read_data, struct fs_input_stream, is);
        
        
        data = (BYTE*)fs_input_stream_get_data_ptr(is);
        len = fs_input_stream_get_len(is);
        
    }
    
    
    if (pack->input_stream != NULL) {
        data = (BYTE*)fs_input_stream_get_data_ptr(pack->input_stream);
        len = fs_input_stream_get_len(pack->input_stream);
    }
    
    
    struct fs_output_stream* fos = fs_create_output_stream_ext;
    fs_stream_write_int32(fos, (int32_t)len - mb_pack_head_len);
    fs_stream_write_data(fos, data, len);
    
    size_t ret_len = fs_output_stream_get_len(fos);
    
    *out = fs_malloc(ret_len);
    memcpy(*out, fs_output_stream_get_dataptr(fos), ret_len);
    
    fs_stream_free_output(fos);
    
    if(len == 0){
        rb_raise(rb_eRuntimeError, "try send data len for 0");
    }
    
    return ret_len;
}




VALUE
protect_fs_ruby_call_func(VALUE argv){
    
    VALUE* argvs = (VALUE*)argv;
    VALUE argc_num = argvs[0];
    VALUE server_instance = argvs[1];
    ID    method_id = argvs[2];
    
    int argc = FIX2INT(argc_num);
    
    if(server_instance == Qnil){
        return Qnil;
    }
    
    
    if (argc == -1) {
        VALUE proc = (VALUE)argvs[1];
        VALUE proc_argv = LL2NUM(argvs[2]);
        VALUE proc_argvs = rb_ary_new();
        rb_ary_push(proc_argvs, proc_argv);
        rb_proc_call(proc, proc_argvs);
        rb_ary_free(proc_argvs);
    }
    
    if(argc == 0){
        rb_funcall(server_instance, method_id, argc, 0);
    }
    
    if(argc == 1){
        VALUE argv = argvs[3];
        rb_funcall(server_instance, method_id, argc, argv);
    }
    
    if(argc == 2){
        VALUE node_id = argvs[3];
        VALUE pack    = argvs[4];
        rb_funcall(server_instance, method_id, argc, node_id, pack);
    }
    
    
    return Qnil;
}


VALUE
protect_fs_ruby_handle_pack(VALUE argv){
    
    VALUE* argvs = (VALUE*)argv;
    
    struct fs_server* server = (struct fs_server*)argvs[0];
    struct fs_pack* pack = (struct fs_pack*)argvs[1];
    
    VALUE server_instance = (VALUE)fs_server_get_script_id(server);
    
    int ret = 0;
    
    switch (pack->pack_type) {
        case fs_sys_pack_type_tick:
        {
            
            VALUE proc = (VALUE)argvs[2];
            VALUE proc_argv = (VALUE)argvs[3];
            
            VALUE argvs[3];
            argvs[0] = INT2FIX(-1);
            argvs[1] = proc;
            argvs[2] = proc_argv;
            
            rb_protect(protect_fs_ruby_call_func, (VALUE)argvs, &ret);
            
        }
            break;
        case fs_sys_pack_type_start:
        {
            VALUE argvs[3];
            argvs[0] = INT2FIX(0);
            argvs[1] = server_instance;
            argvs[2] = rb_intern("on_start");
            rb_protect(protect_fs_ruby_call_func, (VALUE)argvs, &ret);
        }
            break;
        case fs_sys_pack_type_connect:
        {
            
            VALUE argvs[4];
            argvs[0] = INT2FIX(1);
            argvs[1] = server_instance;
            argvs[2] = rb_intern("on_connect_node");
            argvs[3] = INT2FIX(pack->node_id);
            rb_protect(protect_fs_ruby_call_func, (VALUE)argvs, &ret);
        }
            break;
        case fs_sys_pack_type_diconnect:
        {
            VALUE argvs[4];
            argvs[0] = INT2FIX(1);
            argvs[1] = server_instance;
            argvs[2] = rb_intern("on_shudown_node");
            argvs[3] = INT2FIX(pack->node_id);
            rb_protect(protect_fs_ruby_call_func, (VALUE)argvs, &ret);
        }
            break;
        default:
        {
            
            struct fs_node* node = fs_server_find_node_by_id(server, pack->node_id);
            
            fs_bool is_http = fs_server_get_type(server) == t_fs_server_http;
            if( (is_http) || (node && fs_node_is_active(node))){
            
                
                VALUE argv[4];
                argv[0] = INT2FIX(pack->node_id);
                argv[1] = (VALUE)pack->data;
                argv[2] = INT2FIX(pack->len);
                argv[3] = INT2FIX(pack->pack_type);
                VALUE rb_pack = rb_class_new_instance(4, argv, rb_cPack);
                
                VALUE fun_argvs[5];
                fun_argvs[0] = INT2FIX(2);
            
                if(!is_http){
                    // 如果是通过监听进来的,直接去server
                    if(fs_node_is_from_listener(node)) {
                        fun_argvs[1] = server_instance;
                    }
                    // 如果是通过主动连接的, 去指定地方调用
                    if(fs_node_is_from_connect(node)){
                        fun_argvs[1] = fs_node_get_script_id(node);
                    }
                    fun_argvs[2] = rb_intern("on_handle_pack");
                }else{
                    fun_argvs[1] = server_instance;
                    fun_argvs[2] = rb_intern("on_request");
                }
                
                fun_argvs[3] = INT2FIX(pack->node_id);
                fun_argvs[4] = rb_pack;
                
                rb_protect(protect_fs_ruby_call_func, (VALUE)fun_argvs, &ret);
                
                
            }
            
            
            
        }
            break;
    }
    
    
    if(ret != 0){
        rb_p(rb_errinfo());
        rb_eval_string(
                       "for msg in $@; "
                       " printf msg + '\n'; "
                       "end;");
    }

    
    fs_pack_free(pack);
    
    return ret == 0 ? Qtrue : Qfalse;

    
}


fs_bool
fs_ruby_handle_pack(struct fs_server* server, struct fs_pack* pack){
    
    VALUE argv[2];
    argv[0] = (VALUE)server;
    argv[1] = (VALUE)pack;
    
    struct fs_invoke_call_function* invoke = fs_create_invoke_call(protect_fs_ruby_handle_pack, 2, argv);
    fs_ruby_invoke(invoke);
    
    return fs_true;
}

void
fs_ruby_on_server_start( struct fs_server* server ){
    
    fs_server_on_recv_pack(server, fs_create_server_start(server));

}

void
fs_ruby_on_node_connect( struct fs_server* server, fs_id node_id){
    
    fs_server_on_recv_pack(server, fs_create_connect_pack(node_id));
    
}

void
fs_ruby_on_node_shudown( struct fs_server* server, fs_id node_id){

    fs_server_on_recv_pack(server, fs_create_diconnect_pack(node_id));
    
}



void
wrap_Server_free (struct fs_server* server)
{
    fs_server_set_script_id(server, Qnil);
    fs_free(server);
}

VALUE
wrap_Server_allocate (VALUE self)
{
    struct fs_server* p = fs_create_server("ruby_server");
    return Data_Wrap_Struct (self, NULL, wrap_Server_free, p);
}

VALUE
rb_Server_initialize(VALUE self, VALUE v_server_name){
    
    const char* name = StringValueCStr(v_server_name);
    
    struct fs_server* server = NULL;
    Data_Get_Struct(self, struct fs_server, server);
    
    fs_server_set_script_id(server, self);
    
    fs_server_set_name(server, name);
    fs_server_set_handle_pack_fn(server, fs_ruby_handle_pack);
    fs_server_set_on_server_start(server, fs_ruby_on_server_start);
    fs_server_set_node_connect(server, fs_ruby_on_node_connect);
    fs_server_set_node_shudwon(server, fs_ruby_on_node_shudown);
    fs_server_set_parsepack_fn(server, fs_ruby_parse_pack);
    fs_server_set_topack_fn(server, fs_ruby_pack_to_data);
    
    
    return Qnil;
}


VALUE
rb_Server_start_server(VALUE self, VALUE vIP, VALUE vPort, VALUE v_type){
    
    const char* addr = StringValueCStr(vIP);
    int         port = FIX2INT(vPort);
    int         type = FIX2INT(v_type);
    
    struct fs_server* server = NULL;
    Data_Get_Struct(self, struct fs_server, server);
    struct fs_node_addr conf_addr;
    conf_addr.port = port;
    strncpy(conf_addr.addr, addr, 64);
    
    fs_server_start(server, &conf_addr, type);
    
    return Qnil;
}


VALUE
rb_Server_name(VALUE self){
    
    struct fs_server* server = NULL;
    Data_Get_Struct(self, struct fs_server, server);
    
    const char* server_name = fs_server_get_name(server);
    VALUE vName = rb_str_new(server_name, strlen(server_name));
    return vName;
}

VALUE
rb_Server_stop(VALUE self){
    struct fs_server* server = NULL;
    Data_Get_Struct(self, struct fs_server, server);
    fs_server_stop(server, 0);
    return Qnil;
}

void
rb_Server_scheduler_function(struct fs_server* server, unsigned long dt, void* data){
   
    VALUE proc = (VALUE)data;
    
    VALUE argv[4];
    argv[0] = (VALUE)server;
    argv[1] = (VALUE)fs_create_time_tick_pack(server);
    argv[2] = proc;
    argv[3] = (VALUE)dt;
    
    
    struct fs_invoke_call_function* invoke = fs_create_invoke_call(protect_fs_ruby_handle_pack, 4, argv);
    fs_ruby_invoke(invoke);
    
    
}

VALUE
rb_Server_scheduler(VALUE self, VALUE dt, VALUE times, VALUE proc){
    
    
    Check_Type(dt, T_FLOAT);
    struct fs_server* server = NULL;
    Data_Get_Struct(self, struct fs_server, server);
    struct fs_timer* timer = fs_server_scheduler(server, rb_float_value(dt), FIX2INT(times), rb_Server_scheduler_function, (void*)proc);
    
    return ULONG2NUM((unsigned long)timer);
}

VALUE
rb_Server_unscheduler(VALUE self, VALUE timer){
    
    struct fs_timer* ptimer = (struct fs_timer*)NUM2ULONG(timer);
    struct fs_server* server = NULL;
    Data_Get_Struct(self, struct fs_server, server);
    
    fs_bool ret = fs_server_unscheulder(server, ptimer);
    
    return ret ? Qtrue : Qfalse;
}


void
rb_define_fs_net(){
    rb_cFSNet = rb_define_module("FSNET");
    rb_define_module_function(rb_cFSNet, "init", RUBY_METHOD_FUNC(rb_FSNET_init), 0);
    rb_define_module_function(rb_cFSNet, "main_loop", RUBY_METHOD_FUNC(rb_FSNET_mainloop), 0);
}

void
rb_define_fs_server(){
    

    
    
    rb_cServer = rb_define_class("FSServer", rb_cObject);
    
    rb_define_alloc_func(rb_cServer, wrap_Server_allocate);
    rb_define_method(rb_cServer, "initialize", RUBY_METHOD_FUNC(rb_Server_initialize), 1);
    rb_define_method(rb_cServer, "start_server", RUBY_METHOD_FUNC(rb_Server_start_server), 3);
    rb_define_method(rb_cServer, "name", RUBY_METHOD_FUNC(rb_Server_name), 0);
    rb_define_method(rb_cServer, "stop", RUBY_METHOD_FUNC(rb_Server_stop), 0);
    rb_define_method(rb_cServer, "scheduler", RUBY_METHOD_FUNC(rb_Server_scheduler), 3);
    rb_define_method(rb_cServer, "unscheduler", RUBY_METHOD_FUNC(rb_Server_unscheduler), 1);
    
    rb_define_const(rb_cServer, "T_TCP", INT2FIX(t_fs_server_tcp));
    rb_define_const(rb_cServer, "T_HTTP", INT2FIX(t_fs_server_http));
    
    
}





void
wrap_Node_free (struct fs_node* ptr)
{
    if(ptr){
        VALUE vid = fs_node_get_script_id(ptr);
        if (vid != Qnil && RDATA(vid)->data) {
            RDATA(vid)->data = NULL;
            fs_node_set_script_id(ptr, Qnil);
            fs_free(ptr);
        }
    }
}

VALUE
wrap_Node_allocate (VALUE self)
{
    struct fs_node* p = NULL;
    return Data_Wrap_Struct (self, NULL, wrap_Node_free, p);
}

VALUE
rb_Node_initialize(int argc, VALUE* argv, VALUE self){
    
    // connect
    if(argc == 3){
        VALUE server_value = argv[0];
        Check_Type(argv[1], T_STRING);
        Check_Type(argv[2], T_FIXNUM);
        
        struct fs_server* server = NULL;
        Data_Get_Struct(server_value, struct fs_server, server);
        
        
        struct fs_node* node = fs_create_node(server);
        
        struct fs_node_addr addr;
        strcpy(addr.addr, StringValueCStr(argv[1]));
        addr.port = FIX2INT(argv[2]);
        
        
        if(fs_server_connect_node(server, node, &addr)){
            
            RDATA(self)->data = node;
            fs_node_set_script_id(node, self);
            rb_funcall(self, rb_intern("server="), 1, server_value);
            
        }else{
            fs_free(node);
            RDATA(node)->data = NULL;
            rb_raise(rb_eRuntimeError, "connect %s:%d fail", addr.addr, addr.port);
        }
        return Qnil;
    }
        
    // accept
    if(argc == 2){
        Check_Type(argv[0], T_DATA);
        Check_Type(argv[1], T_FIXNUM);
        
        VALUE v_server  = argv[0];
        fs_id node_id = FIX2INT(argv[1]);
        
        struct fs_server* server;
        Data_Get_Struct(v_server, struct fs_server, server);
        
        
        struct fs_node* node = fs_server_find_node_by_id(server, node_id);
        
        if(node){
            RDATA(self)->data = node;
            fs_node_set_script_id(node, self);
            rb_funcall(self, rb_intern("server="), 1, v_server);
        }else{
            
            rb_raise(rb_eRuntimeError, "bind node error");
        }
        return Qnil;
    }
    
    rb_raise(rb_eArgError, "#<ArgumentError: wrong number of arguments (%d for %d)>", argc, 3);
    
    return Qnil;
}


VALUE
rb_Node_send_pack(VALUE self, VALUE argv){
   
    struct fs_node* node = NULL;
    Data_Get_Struct(self, struct fs_node, node);
    if(node && !fs_node_is_closed(node)){
        
        
        VALUE v_server = rb_funcall(self, rb_intern("server"), 0, 0);
        
        struct fs_server* server;
        Data_Get_Struct(v_server, struct fs_server, server);
        
        struct fs_pack* pack = NULL;
        Data_Get_Struct(argv, struct fs_pack, pack);
        
        if(pack && server){
            
            if(fs_server_send_pack_node_by_node(server, node, pack)){
                return Qtrue;
            }
        }
        
        return Qfalse;
    }
    return Qfalse;
}

VALUE
rb_Node_close(VALUE self){
    
    struct fs_node* node = NULL;
    Data_Get_Struct(self, struct fs_node, node);
    if(node) {
        fs_node_shudown(node);
        return Qtrue;
    }else{
        rb_raise(rb_eRuntimeError, "NODE is NULL");
    }
    return Qfalse;
}
VALUE
rb_Node_id(VALUE self){
    struct fs_node* node = NULL;
    Data_Get_Struct(self, struct fs_node, node);
    if(node) {
        return INT2FIX(fs_node_get_id(node));
    }
    return Qnil;
}

VALUE
rb_Node_active(VALUE self){

    return RDATA(self)->data != NULL ? Qtrue : Qfalse;
}
void
rb_define_fs_node(){
    
    rb_cNode = rb_define_class("FSNode", rb_cObject);
    rb_define_alloc_func(rb_cNode, wrap_Node_allocate);
    rb_define_attr(rb_cNode, "server", 1, 1);
    rb_define_method(rb_cNode, "initialize", RUBY_METHOD_FUNC(rb_Node_initialize), -1);
    rb_define_method(rb_cNode, "send_pack", RUBY_METHOD_FUNC(rb_Node_send_pack), 1);
    rb_define_method(rb_cNode, "close", RUBY_METHOD_FUNC(rb_Node_close), 0);
    rb_define_method(rb_cNode, "active", RUBY_METHOD_FUNC(rb_Node_active), 0);
    rb_define_method(rb_cNode, "id", RUBY_METHOD_FUNC(rb_Node_id), 0);
    
    
}




void
wrap_Pack_free (struct fs_pack* ptr)
{
    if(ptr){
        if(ptr->input_stream){
            fs_stream_free_input(ptr->input_stream);
            ptr->input_stream = NULL;
        }
        if(ptr->output_stream){
            fs_stream_free_output(ptr->output_stream);
            ptr->output_stream = NULL;
        }
        
        fs_free(ptr);
    }
}

VALUE
wrap_Pack_allocate (VALUE self)
{
    
    struct fs_pack* p = fs_malloc(sizeof(*p));
    fs_zero(p, sizeof(*p));
    VALUE instance = Data_Wrap_Struct (self, NULL, wrap_Pack_free, p);
    p->script_id = (fs_script_id)instance;
    RSTRING(self)->basic;
    return instance;
}



VALUE
rb_Pack_type (VALUE self)
{
    struct fs_pack* pack = NULL;
    Data_Get_Struct(self, struct fs_pack, pack);
    VALUE input = INT2FIX(pack->pack_type);
    return input;
}


VALUE
rb_Pack_initialize(int argc, VALUE* argv, VALUE self){
    
    
    if (argc == 0) {
        
        struct fs_pack* pack = NULL;
        Data_Get_Struct(self, struct fs_pack, pack);
        rb_funcall(self, rb_intern("write_data="), 1, rb_class_new_instance(0, NULL, rb_cOutputStream));
        
        return Qnil;
        
    }
    
    if(argc == 4){

        fs_id node_id    = FIX2INT(argv[0]);
        const char* data = (const char*)(argv[1]);
        size_t len       = FIX2INT(argv[2]);
        fs_pack_type type = FIX2INT(argv[3]);
    
        struct fs_pack* pack = NULL;
        Data_Get_Struct(self, struct fs_pack, pack);
        pack->node_id = node_id;
        pack->data = (BYTE*)data;
        pack->len = len;
        pack->pack_type = type;
        pack->input_stream = fs_create_input_stream((const BYTE*)data, len);
        
        VALUE v_len  = INT2FIX(fs_input_stream_get_len(pack->input_stream));
        VALUE is_argv[] = { (VALUE)fs_input_stream_get_data_ptr(pack->input_stream), v_len, Qtrue };
        VALUE input_stream = rb_class_new_instance(3, is_argv, rb_cInputStream);
        rb_funcall(self, rb_intern("read_data="), 1, input_stream);
        
        return Qnil;
    }
    
    rb_raise(rb_eArgError, "#<ArgumentError: wrong number of arguments (%d for %d or %d)>", argc, 0, 2);
    
    
    return Qnil;
}

void
rb_define_fs_pack(){
    
    
    rb_cPack = rb_define_class("FSPack", rb_cObject);
    rb_define_alloc_func(rb_cPack, wrap_Pack_allocate);
    rb_define_method(rb_cPack, "initialize", RUBY_METHOD_FUNC(rb_Pack_initialize), -1);
    rb_define_method(rb_cPack, "type",       RUBY_METHOD_FUNC(rb_Pack_type), 0);
    rb_define_attr(rb_cPack, "write_data", 1, 1);
    rb_define_attr(rb_cPack, "read_data", 1, 1);
    
    
    
}






void
wrap_IStream_free (struct fs_input_stream* ptr)
{
    if(ptr){
        fs_stream_free_input(ptr);
    }
    
}
VALUE
wrap_IStream_allocate (VALUE self)
{
    struct fs_input_stream* p = fs_create_input_stream(NULL, 0);
    return Data_Wrap_Struct (self, NULL, wrap_IStream_free, p);
}
VALUE
rb_IStream_initialize(VALUE self, VALUE vData, VALUE vLen, VALUE cPtr){
    const char* data = NULL;
    
    if(cPtr == Qtrue){
        data = (const char*)(vData);
    }else{
        data = StringValuePtr(vData);
    }
    
    size_t len       = FIX2INT(vLen);
    struct fs_input_stream* is = NULL;
    Data_Get_Struct(self, struct fs_input_stream, is);
    
    fs_input_stream_set_data(is, (const BYTE*)data, len);
    
    return Qnil;
}



void
wrap_OStream_free (struct fs_output_stream* ptr)
{
    if(ptr){
        fs_stream_free_output(ptr);
    }
    
}
VALUE
wrap_OStream_allocate (VALUE self)
{
    struct fs_output_stream* p = fs_create_output_stream_ext;
    VALUE instance = Data_Wrap_Struct (self, NULL, wrap_OStream_free, p);
    fs_output_stream_set_script_id(p, instance);
    
    return instance;
}

#define S_READ_FUNC( method_name , function_name, return_pre_fix , stream_type , return_type, return_fix) \
VALUE   \
method_name(VALUE self){   \
struct stream_type* is = NULL; \
Data_Get_Struct(self, struct stream_type, is);  \
return_type _ret_v = function_name(is);\
VALUE ret = return_pre_fix(_ret_v); \
return return_fix; \
}

#define IS_GETER_FUNC( method_name , function_name, return_pre_fix, return_type, _ret, _cl) \
VALUE   \
method_name(VALUE self){   \
struct fs_input_stream* is = NULL; \
Data_Get_Struct(self, struct fs_input_stream, is);  \
if(_cl){\
    if(fs_input_stream_get_pos(is) + sizeof(return_type) > fs_input_stream_get_len(is)){ \
        rb_raise(rb_eEOFError, "input stream eof"); \
    } \
}\
return_type _ret_v = function_name(is);\
VALUE ret = return_pre_fix(_ret_v); \
return _ret; \
}

#define OS_GETER_FUNC( method_name, function_name, return_pre_fix, return_type, _ret) \
        S_READ_FUNC( method_name, function_name, return_pre_fix, fs_output_stream, return_type, _ret)

#define OS_WRITE_FUNC( method_name , function_name, prefix1, prefix , CHECK_TYPE, BE_CHECK)    \
VALUE  \
method_name(VALUE self, VALUE v){ \
if(BE_CHECK) { \
Check_Type(v, CHECK_TYPE);   \
} \
if(TYPE(v) == CHECK_TYPE || !BE_CHECK){ \
struct fs_output_stream* os = NULL; \
Data_Get_Struct(self, struct fs_output_stream, os); \
v = prefix1(v); \
function_name(os, prefix(v)); \
return self; \
}\
return Qnil;  \
}


#define IS_SETTER_FUNC( method_name , function_name, prefix , CHECK_TYPE)    \
VALUE  \
method_name(VALUE self, VALUE v){ \
Check_Type(v, CHECK_TYPE);   \
if(TYPE(v) == CHECK_TYPE){ \
struct fs_input_stream* os = NULL; \
Data_Get_Struct(self, struct fs_input_stream, os); \
function_name(os, prefix(v)); \
return self; \
}\
return Qnil;  \
}

VALUE
rb_IStream_read_string(VALUE self){
    struct fs_input_stream* is = NULL;
    Data_Get_Struct(self, struct fs_input_stream, is);
    if(fs_input_stream_get_pos(is) + sizeof(uint16_t) > fs_input_stream_get_len(is)){
        rb_raise(rb_eEOFError, "input stream eof");
    }
    size_t len = fs_stream_read_uint16(is);
    if(fs_input_stream_get_pos(is) + len > fs_input_stream_get_len(is)){
        rb_raise(rb_eEOFError, "input stream eof");
    }
    char out[len];
    fs_stream_read_data(is, out, len);
    return rb_str_new(out, len);
}
VALUE
rb_IStream_read_data(VALUE self, VALUE vlen){
    struct fs_input_stream* is = NULL;
    Data_Get_Struct(self, struct fs_input_stream, is);
    size_t len = FIX2INT(vlen);
    if(fs_input_stream_get_pos(is) + len > fs_input_stream_get_len(is)){
        rb_raise(rb_eEOFError, "input stream eof");
    }
    char out[len];
    fs_stream_read_data(is, out, len);
    return rb_str_new(out, len);
}

VALUE
rb_OStream_write_string(VALUE self, VALUE v){
    Check_Type(v, T_STRING);
    if(TYPE(v) == T_STRING){
        struct fs_output_stream* os = NULL;
        Data_Get_Struct(self, struct fs_output_stream, os);
        const char* cStr = StringValueCStr(v);
        fs_stream_write_string(os, cStr, strnlen(cStr, 0xffff));
        return self;
    }
    return Qnil;
}

VALUE
rb_OStream_write_data(VALUE self, VALUE v, VALUE len){
    Check_Type(v, T_STRING);
    if(TYPE(v) == T_STRING){
        struct fs_output_stream* os = NULL;
        Data_Get_Struct(self, struct fs_output_stream, os);
        const char* cStr = StringValuePtr(v);
        fs_stream_write_data(os, (BYTE*)cStr, FIX2INT(len));
        return self;
    }
    return Qnil;
}

IS_GETER_FUNC( rb_IStream_read_byte,    fs_stream_read_byte,    INT2FIX ,       BYTE,    ret, fs_true);
IS_GETER_FUNC( rb_IStream_read_long,   fs_stream_read_long,     LONG2NUM ,       long,    ret, fs_true);
IS_GETER_FUNC( rb_IStream_read_ulong,   fs_stream_read_ulong,   ULONG2NUM ,       unsigned long,    ret, fs_true);



#if SIZEOF_LONG_LONG == SIZEOF_LONG
IS_GETER_FUNC( rb_IStream_read_int64,   fs_stream_read_int64,   INT2FIX ,       int64_t, ret, fs_true);
IS_GETER_FUNC( rb_IStream_read_uint64,   fs_stream_read_uint64, INT2FIX ,     uint64_t, ret, fs_true);
#else
IS_GETER_FUNC( rb_IStream_read_int64,   fs_stream_read_int64,   LL2NUM ,       int64_t, ret, fs_true);
IS_GETER_FUNC( rb_IStream_read_uint64,   fs_stream_read_uint64, ULL2NUM ,     uint64_t, ret, fs_true);
#endif

IS_GETER_FUNC( rb_IStream_read_int32,   fs_stream_read_int32,   INT2FIX ,       int32_t, ret, fs_true);
IS_GETER_FUNC( rb_IStream_read_int16,   fs_stream_read_int16,   INT2FIX ,       int16_t, ret, fs_true);
IS_GETER_FUNC( rb_IStream_read_uint32,   fs_stream_read_uint32,   INT2FIX ,       uint32_t, ret, fs_true);
IS_GETER_FUNC( rb_IStream_read_uint16,   fs_stream_read_uint16,   INT2FIX ,       uint16_t, ret, fs_true);
IS_GETER_FUNC( rb_IStream_read_float,   fs_stream_read_float,   rb_float_new ,  float,   ret, fs_true);
IS_GETER_FUNC( rb_IStream_read_double,  fs_stream_read_double,  rb_float_new ,  double,  ret, fs_true);
IS_GETER_FUNC( rb_IStream_pos,          fs_input_stream_get_pos,INT2FIX ,       size_t,  ret, fs_false);
IS_GETER_FUNC( rb_IStream_len,          fs_input_stream_get_len,INT2FIX ,       size_t,  ret, fs_false);
IS_SETTER_FUNC( rb_IStream_skip,         fs_input_stream_skip_to, FIX2INT,       T_FIXNUM);
IS_GETER_FUNC( rb_IStream_data,          fs_input_stream_get_data_ptr,(VALUE) , const BYTE*,  rb_str_new((const char*)ret,fs_input_stream_get_len(is)), fs_false);




OS_WRITE_FUNC( rb_OStream_write_byte, fs_stream_write_byte, rb_to_int, FIX2INT, T_FIXNUM, fs_false);
OS_WRITE_FUNC( rb_OStream_write_long, fs_stream_write_long, rb_to_int, NUM2LONG, T_FIXNUM, fs_false);
OS_WRITE_FUNC( rb_OStream_write_ulong, fs_stream_write_ulong, rb_to_int, NUM2ULONG, T_FIXNUM, fs_false);
OS_WRITE_FUNC( rb_OStream_write_int64, fs_stream_write_int64, rb_to_int, NUM2LL, T_FIXNUM, fs_false);
OS_WRITE_FUNC( rb_OStream_write_uint64, fs_stream_write_uint64, rb_to_int, NUM2LL, T_FIXNUM, fs_false);
OS_WRITE_FUNC( rb_OStream_write_int32, fs_stream_write_int32, rb_to_int, FIX2INT, T_FIXNUM, fs_false);
OS_WRITE_FUNC( rb_OStream_write_int16, fs_stream_write_int16, rb_to_int, FIX2INT, T_FIXNUM, fs_false);
OS_WRITE_FUNC( rb_OStream_write_uint32, fs_stream_write_uint32, rb_to_int, FIX2INT, T_FIXNUM, fs_false);
OS_WRITE_FUNC( rb_OStream_write_uint16, fs_stream_write_uint16, rb_to_int, FIX2INT, T_FIXNUM, fs_false);
OS_WRITE_FUNC( rb_OStream_write_float, fs_stream_write_float, rb_to_float, RFLOAT_VALUE, T_FLOAT, fs_false);
OS_WRITE_FUNC( rb_OStream_write_double, fs_stream_write_double, rb_to_float, RFLOAT_VALUE, T_FLOAT, fs_false);
OS_WRITE_FUNC( rb_OStream_skip,         fs_output_stream_skip_to, rb_to_int, FIX2INT, T_FIXNUM, fs_true);
OS_GETER_FUNC( rb_OStream_len,          fs_output_stream_get_len, INT2FIX , size_t, ret);
OS_GETER_FUNC( rb_OStream_data, fs_output_stream_get_dataptr, (VALUE), const BYTE*, rb_str_new((const char*)ret, fs_output_stream_get_len(is)));



void
rb_define_fs_stream(){
    
    rb_cInputStream = rb_define_class("FSInputStream", rb_cObject);
    rb_define_alloc_func(rb_cInputStream, wrap_IStream_allocate);
    rb_define_method(rb_cInputStream, "initialize", RUBY_METHOD_FUNC(rb_IStream_initialize), 3);
    rb_define_method(rb_cInputStream, "len", RUBY_METHOD_FUNC(rb_IStream_len), 0);
    rb_define_method(rb_cInputStream, "read_byte", RUBY_METHOD_FUNC(rb_IStream_read_byte), 0);
    rb_define_method(rb_cInputStream, "read_int64", RUBY_METHOD_FUNC(rb_IStream_read_int64), 0);
    rb_define_method(rb_cInputStream, "read_uint64", RUBY_METHOD_FUNC(rb_IStream_read_uint64), 0);
    rb_define_method(rb_cInputStream, "read_int32", RUBY_METHOD_FUNC(rb_IStream_read_int32), 0);
    rb_define_method(rb_cInputStream, "read_int16", RUBY_METHOD_FUNC(rb_IStream_read_int16), 0);
    rb_define_method(rb_cInputStream, "read_uint32", RUBY_METHOD_FUNC(rb_IStream_read_uint32), 0);
    rb_define_method(rb_cInputStream, "read_uint16", RUBY_METHOD_FUNC(rb_IStream_read_uint16), 0);
    rb_define_method(rb_cInputStream, "read_float", RUBY_METHOD_FUNC(rb_IStream_read_float), 0);
    rb_define_method(rb_cInputStream, "read_double", RUBY_METHOD_FUNC(rb_IStream_read_double), 0);
    rb_define_method(rb_cInputStream, "read_string", RUBY_METHOD_FUNC(rb_IStream_read_string), 0);
    rb_define_method(rb_cInputStream, "read_long", RUBY_METHOD_FUNC(rb_IStream_read_long), 0);
    rb_define_method(rb_cInputStream, "read_ulong", RUBY_METHOD_FUNC(rb_IStream_read_ulong), 0);
    rb_define_method(rb_cInputStream, "read_data", RUBY_METHOD_FUNC(rb_IStream_read_data), 1);
    rb_define_method(rb_cInputStream, "post", RUBY_METHOD_FUNC(rb_IStream_pos), 0);
    rb_define_method(rb_cInputStream, "data", RUBY_METHOD_FUNC(rb_IStream_data), 0);
    rb_define_method(rb_cInputStream, "post=", RUBY_METHOD_FUNC(rb_IStream_skip), 1);
   
    rb_cOutputStream = rb_define_class("FSOutputStream", rb_cObject);
    rb_define_alloc_func(rb_cOutputStream, wrap_OStream_allocate);
    rb_define_method(rb_cOutputStream, "len", RUBY_METHOD_FUNC(rb_OStream_len), 0);
    rb_define_method(rb_cOutputStream, "post=", RUBY_METHOD_FUNC(rb_OStream_skip), 1);
    rb_define_method(rb_cOutputStream, "data", RUBY_METHOD_FUNC(rb_OStream_data), 0);
    rb_define_method(rb_cOutputStream, "write_byte", RUBY_METHOD_FUNC(rb_OStream_write_byte), 1);
    rb_define_method(rb_cOutputStream, "write_int64", RUBY_METHOD_FUNC(rb_OStream_write_int64), 1);
    rb_define_method(rb_cOutputStream, "write_uint64", RUBY_METHOD_FUNC(rb_OStream_write_uint64), 1);
    rb_define_method(rb_cOutputStream, "write_int32", RUBY_METHOD_FUNC(rb_OStream_write_int32), 1);
    rb_define_method(rb_cOutputStream, "write_int16", RUBY_METHOD_FUNC(rb_OStream_write_int16), 1);
    rb_define_method(rb_cOutputStream, "write_uint32", RUBY_METHOD_FUNC(rb_OStream_write_uint32), 1);
    rb_define_method(rb_cOutputStream, "write_uint16", RUBY_METHOD_FUNC(rb_OStream_write_uint16), 1);
    rb_define_method(rb_cOutputStream, "write_float", RUBY_METHOD_FUNC(rb_OStream_write_float), 1);
    rb_define_method(rb_cOutputStream, "write_double", RUBY_METHOD_FUNC(rb_OStream_write_double), 1);
    rb_define_method(rb_cOutputStream, "write_long", RUBY_METHOD_FUNC(rb_OStream_write_long), 1);
    rb_define_method(rb_cOutputStream, "write_ulong", RUBY_METHOD_FUNC(rb_OStream_write_ulong), 1);
    rb_define_method(rb_cOutputStream, "write_string", RUBY_METHOD_FUNC(rb_OStream_write_string), 1);
    rb_define_method(rb_cOutputStream, "write_data", RUBY_METHOD_FUNC(rb_OStream_write_data), 2);
}

VALUE
rb_HTTPRequest_response(VALUE self, VALUE argv){
    
    VALUE evhttp_p = rb_funcall(self, rb_intern("magic_request_id"), 0);
    
    VALUE r_code = rb_funcall(argv, rb_intern("code"), 0);
    VALUE r_data = rb_funcall(argv, rb_intern("data"), 0);
    VALUE r_header = rb_funcall(argv, rb_intern("headers"), 0);
    
    Check_Type(r_code, T_FIXNUM);
    Check_Type(r_data, T_STRING);
    Check_Type(r_header, T_ARRAY);
    
    long i = 0;
    struct evbuffer *buf;
    struct evhttp_request* req = (struct evhttp_request*)NUM2ULONG(evhttp_p);
    
    buf = evbuffer_new();
    evbuffer_add_printf(buf, "%s", StringValueCStr(r_data));
    
    for(i = 0 ; i < RARRAY_LEN(r_header) ; i++){
        VALUE header = RARRAY_AREF(r_header, i);
        VALUE key = rb_funcall(header, rb_intern("key"), 0);
        VALUE value = rb_funcall(header, rb_intern("value"), 0);
        evhttp_add_header(req->output_headers, StringValueCStr(key), StringValueCStr(value));
    }
    
    evhttp_send_reply(req, FIX2INT(r_code), "OK", buf);
    evbuffer_free(buf);
    
    return self;

}


void
rb_define_fs_http(){
    
    
    rb_cHTTPRequest = rb_define_class("HTTPRequest", rb_cObject);
    
    rb_define_method(rb_cHTTPRequest, "response", RUBY_METHOD_FUNC(rb_HTTPRequest_response), 1);
    
    rb_cHTTPResponse = rb_define_class("HTTPResponse", rb_cObject);
    
}


void Init_fsnet(){
    
    rb_define_fs_net();
    rb_define_fs_server();
    rb_define_fs_node();
    rb_define_fs_pack();
    rb_define_fs_stream();
    rb_define_fs_http();
    
    
}


void
fs_rb_init(int argc,  char** argv){
    
    ruby_sysinit(&argc, &argv);
    RUBY_INIT_STACK
    ruby_init();
    ruby_init_loadpath();
    ruby_set_argv(argc, argv);
    Init_fsnet();
    
}

