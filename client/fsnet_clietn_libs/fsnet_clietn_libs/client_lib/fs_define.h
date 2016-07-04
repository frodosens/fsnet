//
//  fs_define.h
//  fsnet
//
//  Created by Vincent on 14-5-20.
//  Copyright (c) 2014年 Vincent. All rights reserved.
//

#ifndef FSNet_fs_define_h
#define FSNet_fs_define_h


#ifdef __cplusplus
extern "C" {
#endif
#include <stdio.h>
#include <string.h>
#include <stdlib.h>


#define fs_byte unsigned char
#define fs_bool fs_byte
#define fs_true 1
#define fs_false 0
#define fs_id int
//#define BYTE fs_byte

#define fs_unused(VAR) (void)&(VAR)
    
#ifdef __cplusplus
}
#endif
    

#endif
