#pragma once
#include <charconv>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>

namespace fsnet {
// A transport-independent client session. Only trusted transport/delegation
// code supplies it; possession of its serialized value grants no authority.
struct ConnectionRef {
    std::string id;
    std::uint64_t generation=0;
    bool operator==(const ConnectionRef&) const=default;
    explicit operator bool()const{return valid();}
    bool valid()const {
        if(id.size()!=32 || !generation)return false;
        for(char c:id)if(!((c>='0'&&c<='9')||(c>='a'&&c<='f')))return false;
        return true;
    }
    std::string key()const {
        if(!valid())throw std::invalid_argument("invalid logical connection");
        return "connection/"+id+'/'+std::to_string(generation);
    }
    static ConnectionRef from_key(std::string_view key) {
        if(!key.starts_with("connection/"))throw std::invalid_argument("invalid connection key");
        key.remove_prefix(11);if(key.size()<34 || key[32]!='/')throw std::invalid_argument("invalid connection key");
        ConnectionRef result{std::string(key.substr(0,32)),0};key.remove_prefix(33);
        auto [end,error]=std::from_chars(key.data(),key.data()+key.size(),result.generation);
        if(error!=std::errc{} || end!=key.data()+key.size() || !result.valid() || (key.size()>1 && key.front()=='0'))throw std::invalid_argument("invalid connection generation");
        return result;
    }
    static ConnectionRef create();
};
}
