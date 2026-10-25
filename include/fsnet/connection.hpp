#pragma once
#include <charconv>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>

namespace fsnet {
inline bool valid_connection_key(std::string_view key) {
    if(!key.starts_with("connection/") || key.size()<45 || key.size()>64)return false;
    key.remove_prefix(11);
    for(unsigned i=0;i<32;++i)if(!((key[i]>='0'&&key[i]<='9')||(key[i]>='a'&&key[i]<='f')))return false;
    if(key[32]!='/')return false;
    key.remove_prefix(33);
    if(key.empty() || key.front()=='0')return false;
    std::uint64_t generation=0;auto [end,error]=std::from_chars(key.data(),key.data()+key.size(),generation);
    return error==std::errc{} && end==key.data()+key.size() && generation;
}
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
    bool matches_key(std::string_view key)const noexcept {
        if(!valid() || !valid_connection_key(key))return false;
        key.remove_prefix(11);if(key.substr(0,32)!=id)return false;
        key.remove_prefix(33);std::uint64_t value=0;
        auto [end,error]=std::from_chars(key.data(),key.data()+key.size(),value);
        return error==std::errc{} && end==key.data()+key.size() && value==generation;
    }
    static ConnectionRef from_key(std::string_view key) {
        if(!valid_connection_key(key))throw std::invalid_argument("invalid connection key");
        key.remove_prefix(11);if(key.size()<34 || key[32]!='/')throw std::invalid_argument("invalid connection key");
        ConnectionRef result{std::string(key.substr(0,32)),0};key.remove_prefix(33);
        auto [end,error]=std::from_chars(key.data(),key.data()+key.size(),result.generation);
        if(error!=std::errc{} || end!=key.data()+key.size() || !result.valid() || (key.size()>1 && key.front()=='0'))throw std::invalid_argument("invalid connection generation");
        return result;
    }
    static ConnectionRef create();
};
}
