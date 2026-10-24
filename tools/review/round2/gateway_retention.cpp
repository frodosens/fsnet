#include "fsnet/client_gateway.hpp"
#include "fsnet/entity_message.hpp"
#include <condition_variable>
#include <fstream>
#include <iostream>
#include <mutex>
#include <thread>
using namespace std::chrono_literals;
using Clock=std::chrono::steady_clock;
struct BlockingDb:fsnet::Storage {
 std::mutex mutex;std::condition_variable cv;bool blocked=true;std::size_t reads=0;
 std::optional<fsnet::Record> get(std::string_view) override {std::unique_lock lock(mutex);++reads;cv.wait(lock,[&]{return !blocked;});return {};}
 bool compare_exchange(std::string_view,std::uint64_t,std::string_view)override{return false;}
 bool erase(std::string_view,std::uint64_t)override{return false;}
 void unblock(){std::lock_guard lock(mutex);blocked=false;cv.notify_all();}
};
std::size_t rss(){std::ifstream f("/proc/self/status");std::string key;while(f>>key){if(key=="VmRSS:"){std::size_t value;f>>value;return value;}std::string line;std::getline(f,line);}return 0;}
int main(){
 auto db=std::make_shared<BlockingDb>();
 fsnet::Node front("x"),back("y"),client("x");
 front.identify([](auto,const auto&){return std::optional(fsnet::CallIdentity{fsnet::Caller::client,"alice"});});
 fsnet::GatewayLimits limits;limits.requests=4;limits.bytes=1024*1024;limits.request_timeout=50ms;
 fsnet::ClientGateway gateway(front,back,db,{{"owner","127.0.0.1",1,{"Actor"}}},limits);gateway.bind_entity("Actor",{"echo"});
 auto peer=client.connect("127.0.0.1",front.listen("127.0.0.1",0));
 auto pump=[&]{front.poll();back.poll();client.poll();gateway.poll();};
 auto initial=Clock::now()+100ms;while(Clock::now()<initial){pump();std::this_thread::sleep_for(1ms);}
 auto baseline=rss();std::size_t completed=0,timeouts=0;auto data=fsnet::entity_message("alice",std::string(240*1024,'x'));
 for(unsigned round=1;round<=30;++round){
  for(unsigned i=0;i<4;++i)client.call(peer,"Actor.echo",data,1s,[&](auto result){++completed;if(result.status==fsnet::Status::timeout)++timeouts;});
  auto end=Clock::now()+2s;while(completed<round*4&&Clock::now()<end){pump();std::this_thread::sleep_for(1ms);}
  auto s=gateway.stats();if(round==1||round==10||round==20||round==30)std::cout<<"{\"round\":"<<round<<",\"completed\":"<<completed<<",\"timeouts\":"<<timeouts<<",\"pending\":"<<s.pending<<",\"queued_bytes\":"<<s.queued_bytes<<",\"rss_kib\":"<<rss()<<",\"rss_delta_kib\":"<<rss()-baseline<<"}"<<std::endl;
 }
 db->unblock();for(unsigned i=0;i<100;++i){pump();std::this_thread::sleep_for(1ms);}gateway.stop();
}
