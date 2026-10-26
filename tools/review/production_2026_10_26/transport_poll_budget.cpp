#include "fsnet/node.hpp"
#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>
#include <atomic>
#include <chrono>
#include <iostream>
#include <thread>
using Clock=std::chrono::steady_clock;using namespace std::chrono_literals;
std::string frame(unsigned kind,std::string route,std::string data){std::string out;auto put=[&](uint64_t x,unsigned n){for(unsigned i=n;i;--i)out+=char(x>>((i-1)*8));};put(14+route.size()+data.size(),4);put(1,1);put(kind,1);put(0,2);put(0,8);put(route.size(),2);out+=route;out+=data;return out;}
int main(){fsnet::Node node("t");auto port=node.listen("127.0.0.1",0);int fd=socket(AF_INET,SOCK_STREAM,0);sockaddr_in peer{};peer.sin_family=AF_INET;peer.sin_port=htons(port);inet_pton(AF_INET,"127.0.0.1",&peer.sin_addr);if(connect(fd,(sockaddr*)&peer,sizeof(peer)))return 2;timeval timeout{0,100000};setsockopt(fd,SOL_SOCKET,SO_SNDTIMEO,&timeout,sizeof(timeout));auto hello=frame(0,"","t");send(fd,hello.data(),hello.size(),MSG_NOSIGNAL);std::atomic<bool> done=false;std::atomic<uint64_t> sent=0,received=0;node.on_notification("n",[&](auto,const auto&,auto){++received;});
std::thread sender([&]{auto f=frame(3,"n","");std::string chunk;for(unsigned i=0;i<16384;++i)chunk+=f;auto end=Clock::now()+2s;std::size_t offset=0;while(Clock::now()<end){auto n=send(fd,chunk.data()+offset,chunk.size()-offset,MSG_NOSIGNAL);if(n>0){sent+=n;offset=(offset+n)%chunk.size();}else break;}done=true;});double worst=0;uint64_t polls=0;auto end=Clock::now()+3s;while((!done||Clock::now()<end)&&Clock::now()<end){auto start=Clock::now();node.poll();worst=std::max(worst,std::chrono::duration<double,std::milli>(Clock::now()-start).count());++polls;}sender.join();close(fd);node.stop();std::cout<<"polls="<<polls<<" worst_poll_ms="<<worst<<" notifications="<<received<<" sent_bytes="<<sent<<'\n';}
