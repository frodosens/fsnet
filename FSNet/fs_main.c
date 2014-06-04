//
//  main.c
//  fsnet
//
//  Created by Vincent on 14-5-20.
//  Copyright (c) 2014年 Vincent. All rights reserved.
//

#include <stdio.h>
#include <unistd.h>
#include <string.h>

#include "fsnet/ruby_define/rb_define.h"


int main(int argc,  char * argv[])
{
    
    fs_rb_init(argc, argv);
    
    int pathc = 4;
    const char* paths[] = {
        "/Users/kay/Documents/xcode/FSNet/FSNet",
        "/Users/kay/Documents/xcode/FSNet/FSNet/rubylib",
        "/Users/kay/Documents/xcode/FSNet/FSNet/game_server",
        "/usr/local/lib/ruby/2.1.0"
    };
    
    fs_rb_loop("main.rb", pathc, paths);
    
    printf(":) \n");
    
    
    return 0;
}

