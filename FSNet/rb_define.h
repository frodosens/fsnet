//
//  rb_define.h
//  fsnet
//
//  Created by Vincent on 14-5-23.
//  Copyright (c) 2014年 Vincent. All rights reserved.
//

#ifndef fsnet_rb_define_h
#define fsnet_rb_define_h

#include <stdio.h>
#include <ruby.h>

struct fs_invoke_call_function{
    VALUE* argv;
    int    argc;
    VALUE (*func)(VALUE);
};

void fs_rb_init(int argc,  char** argv);
void fs_rb_start(const char* main_file, int pathc, const char** pathv);
int fs_ruby_invoke(struct fs_invoke_call_function*);
struct fs_invoke_call_function* fs_ruby_pop_call_invoke();

VALUE rb_FSNET_init(VALUE);
VALUE rb_FSNET_mainloop(VALUE);





#endif
