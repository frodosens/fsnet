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


void fs_rb_init(int argc,  char** argv);
void fs_rb_loop(const char* main_file, int pathc, const char** pathv);

int main(int argc,  char * argv[])
{
    
    fs_rb_init(argc, argv);
    
    int pathc = 1;
    const char* paths[] = {
        "/Users/kay/Documents/xcode/FSNet/FSNet",
        "/Users/kay/Documents/xcode/FSNet/FSNet/rubylib",
        "/Users/kay/Documents/xcode/FSNet/FSNet/game_server",
        "/usr/local/lib/ruby/2.1.0"
    };
    
    fs_rb_loop(argc == 0 ? "main.rb" : argv[1], pathc, paths);
    
    printf(":) \n");
    
    
    return 0;
}


