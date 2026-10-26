#include "fsnet/entity_router.hpp"
#include "fsnet/entity_message.hpp"
#include <iostream>
#include <thread>
using namespace fsnet;using namespace std::chrono_literals;
int main(){try{
 Node owner("review"),caller("review");auto db=memory_database();EntityCatalog catalog(*db);auto id=catalog.create("review","slow",{"Actor",{},"","owner"});
 Reply delayed;std::chrono::steady_clock::time_point due;
 owner.bind_rpc("@",{true,true,false},[&](const auto&,std::string,Reply reply){delayed=std::move(reply);due=std::chrono::steady_clock::now()+1500ms;});
 auto port=owner.listen("127.0.0.1",0);EntityRouter router(caller,db,{{"owner","127.0.0.1",port,{"Actor"}}});
 auto pump=[&]{owner.poll();caller.poll();router.poll();if(delayed&&std::chrono::steady_clock::now()>=due){auto r=std::move(delayed);r({Status::ok,"completed"});}};
 auto invoke=[&](bool routed){std::optional<Result> result;auto begin=std::chrono::steady_clock::now();auto finish=[&](Result r){result=std::move(r);};
  if(routed)router.call(id,"slow","",3s,finish);else{auto peer=caller.connect("127.0.0.1",port);caller.call(peer,"@",entity_call_message(id,"slow",""),3s,finish);}
  while(!result && std::chrono::steady_clock::now()-begin<4s){pump();std::this_thread::sleep_for(1ms);}
  if(!result)throw std::runtime_error("deadline");
  std::cout<<(routed?"router":"direct")<<" requested_ms=3000 status="<<unsigned(result->status)<<" elapsed_ms="<<std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now()-begin).count()<<'\n';
 };
 invoke(false);invoke(true);router.stop();caller.stop();owner.stop();
}catch(const std::exception&e){std::cerr<<e.what()<<'\n';return 1;}}
