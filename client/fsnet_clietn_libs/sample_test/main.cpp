//
//  main.cpp
//  sample_test
//
//  Created by Vincent on 16/7/3.
//  Copyright © 2016年 Vincent. All rights reserved.
//

#include <iostream>
#include "FSNet.h"

int main(int argc, const char * argv[]) {
    

    FSClientImpl client("127.0.0.1", (uint16_t)5000);
    
    
    client.connect();
    
    while(1){
    
    
        client.tick(1);
        
        sleep(1);
    }
    
    return 0;
}
