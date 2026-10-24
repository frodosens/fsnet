#include "fsnet/entity_host.hpp"
#include "fsnet/entity_message.hpp"
#include <atomic>
#include <chrono>
#include <future>
#include <iostream>
#include <thread>
#include <unordered_map>
using namespace std::chrono_literals;
struct Host : fsnet::ScriptHost {
 std::unordered_map<std::string, fsnet::Handler> handlers;
 bool has_route(const std::string& r) const override { return handlers.contains(r); }
 void bind(std::string r, fsnet::Handler h) override { handlers.emplace(std::move(r), std::move(h)); }
 void unbind(const std::string& r) override { handlers.erase(r); }
 std::uint64_t connect(const std::string&, std::uint16_t) override { return 1; }
 void call(std::uint64_t, std::string, std::string, std::chrono::milliseconds, fsnet::Reply r) override { r({fsnet::Status::ok,{}}); }
};
struct Blocking : fsnet::Storage {
 std::shared_ptr<fsnet::Storage> db = fsnet::memory_database();
 std::promise<void> entered, release;
 std::shared_future<void> gate = release.get_future().share();
 bool first = true;
 std::optional<fsnet::Record> get(std::string_view k) override { if(first) { first=false; entered.set_value(); gate.wait(); } return db->get(k); }
 bool compare_exchange(std::string_view k, std::uint64_t v, std::string_view val) override { return db->compare_exchange(k,v,val); }
 bool erase(std::string_view k, std::uint64_t v) override { return db->erase(k,v); }
};
int main() {
 auto p=std::make_shared<Host>(); auto db=std::make_shared<Blocking>();
 fsnet::EntityScriptHost host(p, db, "owner/boot");
 host.bind_stateful("Counter.add", "Counter", "0", [](std::string, fsnet::Reply r){r({fsnet::Status::ok,""});});
 bool replied=false;
 p->handlers.at("Counter.add")(fsnet::entity_message("1", ""),[&](auto){replied=true;});
 host.poll();
 if(db->entered.get_future().wait_for(2s)!=std::future_status::ready) return 2;
 std::thread release([&]{ std::this_thread::sleep_for(50ms); db->release.set_value(); });
 host.stop(); release.join();
 std::cout << "stopped=" << replied << '\n';
}
