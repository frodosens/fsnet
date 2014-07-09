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
#include <pthread.h>


void fs_rb_init(int argc,  char** argv);
void fs_rb_loop(const char* main_file, int pathc, const char** pathv);
void* recv_stdin(void*);


int main(int argc,  char * argv[])
{
    
    fs_rb_init(argc, argv);
    
    pthread_t stdin_thread;
    pthread_create(&stdin_thread, NULL, recv_stdin, NULL);
    

    
    int pathc = 1;
    const char* main_rb = "main.rb";
    const char* paths[] = {
        "/Users/kay/Documents/xcode/FSNet/FSNet/scripts",
    };
    
    if(argc > 1){
        main_rb = argv[1];
    }
    
    fs_rb_loop(main_rb, pathc, paths);
        
    printf(":) \n");
    
    
    return 0;
}

void* recv_stdin(void* data){
    
    return NULL;
    
}
