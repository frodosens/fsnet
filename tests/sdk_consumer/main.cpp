#include <fsnet/client.hpp>
#ifdef TEST_LUA
#include <fsnet/lua_engine.hpp>
#endif
int main(){
    fsnet::ClientOptions options;options.token="installed-sdk";
    fsnet::Client client(std::move(options));
    if(client.state()!=fsnet::ClientState::disconnected)return 1;
#ifdef TEST_LUA
    fsnet::Node node("installed-lua");
    auto host=std::make_shared<fsnet::NodeScriptHost>(node);
    fsnet::LuaEngine engine(host);engine.load("assert(type(fsnet)=='table')");engine.stop();
#endif
}
