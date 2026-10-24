#include "fsnet/storage.hpp"
#include <chrono>
#include <filesystem>
#include <iostream>
int main(int argc,char**argv) {
 auto root=std::filesystem::path(argv[1]); auto db=fsnet::file_storage(root.string());
 auto before=std::chrono::steady_clock::now();
 for(int i=0;i<4096;++i) if(db->get("absent-"+std::to_string(i))) return 2;
 unsigned locks=0,records=0,shards=0;
 for(auto& f:std::filesystem::recursive_directory_iterator(root)) {
  if(f.is_directory())++shards;
  else if(f.path().extension()==".lock")++locks;
  else if(f.path().extension()==".record")++records;
 }
 std::cout<<"misses=4096 locks="<<locks<<" records="<<records<<" shards="<<shards<<" elapsed_ms="<<std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-before).count()<<'\n';
}
