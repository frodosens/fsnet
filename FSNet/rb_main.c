//
//  rb_main.c
//  fsnet
//
//  Created by Vincent on 14-5-26.
//  Copyright (c) 2014年 Vincent. All rights reserved.
//

#include "rb_define.h"
#include "fs_loop_queue.h"
#include "fs_malloc.h"
#include <pthread.h>
#include <signal.h>


#define INVOKE_LEN 20480

pthread_mutex_t pthread_ruby_invoke_call_invoke_mutex;
pthread_cond_t pthread_ruby_invoke_call_invoke_cond;

struct fs_loop_queue* ruby_invoke_loop_que;



int evthread_use_pthreads(void);
void event_set_mem_functions(
                             void *(*malloc_fn)(size_t sz),
                             void *(*realloc_fn)(void *ptr, size_t sz),
                             void (*free_fn)(void *ptr));


void
fs_rb_print_error(){
    int ret = 0;
    rb_p(rb_errinfo());
    rb_eval_string_protect("if($@ != nil) ;"
                           "for msg in $@; "
                            " printf msg + '\n'; "
                           "end;"
                           "end", &ret);
}

void
fs_rb_start(const char* main_file, int pathc, const char** pathv){

    
    int i = 0;
    int ret = 0;
    for(i = 0 ; i < pathc ; i++){
        char path[128];
        snprintf(path, 128, " $: << '%s' ", pathv[i]);
        rb_eval_string(path);
    }
    
    rb_eval_string("ARGV << '-xcode'");
    
    ruby_show_version();
    ruby_script("fsnet");
    rb_load_protect(rb_str_buf_new_cstr(main_file), 0, &ret);
    
    if(ret != 0){
        fs_rb_print_error();
        return;
    }
    
}


int
fs_ruby_invoke(struct fs_invoke_call_function* invoke){
    
    pthread_mutex_lock(&pthread_ruby_invoke_call_invoke_mutex);
    fs_bool ret = fs_false;
    ret = fs_loop_queue_push(ruby_invoke_loop_que, invoke);
    if(ret){
        pthread_cond_signal(&pthread_ruby_invoke_call_invoke_cond);
    }else{
        do{
            ret = fs_loop_queue_push(ruby_invoke_loop_que, invoke);
            pthread_mutex_unlock(&pthread_ruby_invoke_call_invoke_mutex);
            pthread_cond_signal(&pthread_ruby_invoke_call_invoke_cond);
            usleep(5000);
        }while (!ret);
    }
    
    pthread_mutex_unlock(&pthread_ruby_invoke_call_invoke_mutex);
    
    return fs_true;
}


struct
fs_invoke_call_function* fs_ruby_pop_call_invoke(){
    
    struct fs_invoke_call_function* func = (struct fs_invoke_call_function*)fs_loop_queue_pop(ruby_invoke_loop_que);
    
    return func;
    
    
}


VALUE
rb_FSNET_init(VALUE self){
    
    pthread_mutex_init(&pthread_ruby_invoke_call_invoke_mutex, NULL);
    pthread_cond_init(&pthread_ruby_invoke_call_invoke_cond, NULL);
    ruby_invoke_loop_que = fs_create_loop_queue(INVOKE_LEN);
    
    evthread_use_pthreads();
    event_set_mem_functions(fs_malloc, fs_realloc, fs_free);
    
    sigignore( SIGPIPE );
    
    return Qtrue;
}

VALUE rb_FSNET_mainloop(VALUE self){

    
    struct fs_invoke_call_function* invoke = NULL;
    
    do{
        
    retry:
        while((invoke = fs_ruby_pop_call_invoke()) != NULL){
            int ret = 0;
            invoke->func((VALUE)invoke->argv);
            fs_free(invoke->argv);
            fs_free(invoke);
            
            if(ret != 0){
                fs_rb_print_error();
            }
        }
        pthread_mutex_lock(&pthread_ruby_invoke_call_invoke_mutex);
        
        // 过了临界点.发现有数据,就回去
        // 效率 * 2
        if(!fs_loop_queue_empty(ruby_invoke_loop_que)){
            pthread_mutex_unlock(&pthread_ruby_invoke_call_invoke_mutex);
            goto retry;
        }
        
        pthread_cond_wait(&pthread_ruby_invoke_call_invoke_cond,
                          &pthread_ruby_invoke_call_invoke_mutex);
        
        pthread_mutex_unlock(&pthread_ruby_invoke_call_invoke_mutex);
        
        
    }while (fs_true);
    
//    ruby_show_copyright();
    
    return Qtrue;
}
