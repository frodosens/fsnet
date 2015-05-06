//
//  ViewController.m
//  fsnet_client
//
//  Created by Vincent on 14/12/21.
//  Copyright (c) 2014年 Vincent. All rights reserved.
//

#import "ViewController.h"

#include "FSChannelFactory.h"
#include "FSClientImpl.h"
#include "LoginChannel.h"


static FSClientImpl* s_client;
@implementation ViewController

- (void)viewDidLoad {
    [super viewDidLoad];
    
    
    
    FACTORY_BIND_CHANNEL(LoginChannel);
    
    s_client = new FSClientImpl("127.0.0.1", 40000);
    if(s_client->connect()){
        while (true) {
            s_client->tick(0);
        }
    }else{
        UIAlertView* view = [[UIAlertView alloc] initWithTitle:@"" message:@"连接失败" delegate:nil cancelButtonTitle:@"OK" otherButtonTitles:nil, nil];
        [view show];
    }
}

- (void)didReceiveMemoryWarning {
    [super didReceiveMemoryWarning];
}





@end
