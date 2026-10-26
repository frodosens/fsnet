#include "fsnet/async_database.hpp"
#include <iostream>
#include <thread>
#include <chrono>
using namespace std::chrono_literals;
int main(int argc,char**argv){try{auto db=fsnet::postgres_database(argv[1]);fsnet::AsyncDatabase executor(db);std::cout<<"ready"<<std::endl;char signal;std::cin.get(signal);executor.submit(1,[](fsnet::Storage& db){std::cout<<"operation_enter"<<std::endl;(void)db.get("blackhole-probe");std::cout<<"operation_return"<<std::endl;},[](auto){});std::this_thread::sleep_for(100ms);std::cout<<"stop_enter"<<std::endl;executor.stop();std::cout<<"stop_return"<<std::endl;}catch(const std::exception&e){std::cerr<<e.what()<<std::endl;return 1;}}
