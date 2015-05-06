//
//  md5.h
//  GS55ClientLib
//
//  Created by Vincent on 14/11/25.
//  Copyright (c) 2014年 Vincent. All rights reserved.
//

#ifndef GS55ClientLib_md5_h
#define GS55ClientLib_md5_h


#include "fs_define.h"


#ifdef __cplusplus
extern "C" {
#endif
void str2md5(const char* input, size_t len, unsigned char* out);

    
#ifdef __cplusplus
}
#endif
    
#endif
