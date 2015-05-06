//
//  LoginChannel.h
//  fsnet_client
//
//  Created by Vincent on 15/5/5.
//  Copyright (c) 2015年 Vincent. All rights reserved.
//

#ifndef __fsnet_client__LoginChannel__
#define __fsnet_client__LoginChannel__

#include <stdio.h>

#include "FSChannel.h"
#include "FSChannelFactory.h"

class LoginChannel : public FSChannel{

    
public:
    virtual void bindMethods();
    
    
public:
    LOCAL_METHOD void init( const FSParams* );
    LOCAL_METHOD void login_ret( const FSParams* );
    
    
    REMOTE_METHOD void login(const char* user_name, const char* user_pwd);
    
    
    REGIST_CHANNEL(LoginChannel);

};

#endif /* defined(__fsnet_client__LoginChannel__) */
