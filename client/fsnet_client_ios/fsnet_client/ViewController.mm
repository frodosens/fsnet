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


@implementation ViewController

- (void)viewDidLoad {
    [super viewDidLoad];
    
    
    
    FACTORY_BIND_CHANNEL(LoginChannel);
    
    const int client_count = 1;
    FSClientImpl* clients[client_count];
    for(int i = 0 ; i < client_count ; i++){
        clients[i] = new FSClientImpl("127.0.0.1", 40000);
        clients[i]->connect();
    }
    while (true) {
        for(int i = 0 ;i  <client_count ; i++){
            clients[i]->tick(0);
        }
    }
}

- (void)didReceiveMemoryWarning {
    [super didReceiveMemoryWarning];
}





@end
