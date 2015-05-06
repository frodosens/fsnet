//
//  base64.h
//  GS55ClientLib
//
//  Created by Vincent on 14/11/28.
//  Copyright (c) 2014年 Vincent. All rights reserved.
//

#ifndef __GS55ClientLib__base64__
#define __GS55ClientLib__base64__

#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif


char* base64_encode( const unsigned char * bindata, char * base64, int binlength );
int base64_decode( const char * base64, unsigned char * bindata );

    
#ifdef __cplusplus
}
#endif

    
#endif /* defined(__GS55ClientLib__base64__) */
