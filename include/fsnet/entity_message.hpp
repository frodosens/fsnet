#pragma once
#include <stdexcept>
#include <string>
#include <string_view>
#include <optional>
#include <cstdint>
#include "fsnet/entity_id.hpp"

namespace fsnet {
inline constexpr const char* entity_call_route="@";
inline bool valid_entity_method(std::string_view method) {
    return !method.empty() && method.size()<=127 && method.front()!='$' && method.find_first_of(".\0",0,2)==std::string_view::npos;
}
// FSE3: u64 logical ID, flags, u8 method length/method, optional u8 request
// length/request and u8 expected-type length/type, then opaque bytes. Both
// clients and server routers use this codec; the outer Node route is "@".
inline std::string entity_call_message(EntityId id,std::string_view method,std::string_view payload,
                                      std::string_view request_id={},std::string_view expected_type={}) {
    if(!valid_entity_method(method) || request_id.size()>128 ||
       (!expected_type.empty() && (expected_type.size()>127 || expected_type.find_first_of(".\0",0,2)!=std::string_view::npos)))
        throw std::invalid_argument("invalid global entity call");
    std::string out="FSE3";out.reserve(14+method.size()+payload.size()+request_id.size()+expected_type.size()+2);
    for(int shift=56;shift>=0;shift-=8)out.push_back(static_cast<char>(id.value()>>shift));
    out.push_back(static_cast<char>((request_id.empty()?0:1)|(expected_type.empty()?0:2)));
    out.push_back(static_cast<char>(method.size()));out.append(method);
    if(!request_id.empty()){out.push_back(static_cast<char>(request_id.size()));out.append(request_id);}
    if(!expected_type.empty()){out.push_back(static_cast<char>(expected_type.size()));out.append(expected_type);}
    out.append(payload);return out;
}
struct EntityCallView {EntityId id;std::string_view method,payload,request_id,expected_type;};
inline std::optional<EntityCallView> parse_entity_call_view(std::string_view input) {
    if(input.size()<15 || !input.starts_with("FSE3"))return {};
    input.remove_prefix(4);std::uint64_t value=0;
    for(unsigned i=0;i<8;++i)value=(value<<8)|static_cast<unsigned char>(input[i]);
    input.remove_prefix(8);if(!value)return {};
    auto flags=static_cast<unsigned char>(input.front());input.remove_prefix(1);if(flags>3)return {};
    auto text=[&](std::size_t maximum)->std::optional<std::string_view>{
        if(input.empty())return {};
        auto n=static_cast<unsigned char>(input.front());input.remove_prefix(1);
        if(!n || n>maximum || n>input.size())return {};
        auto part=input.substr(0,n);input.remove_prefix(n);return part;
    };
    auto method=text(127);if(!method || !valid_entity_method(*method))return {};
    std::string_view request,type;
    if(flags&1){auto part=text(128);if(!part)return {};request=*part;}
    if(flags&2){auto part=text(127);if(!part || part->find_first_of(".\0",0,2)!=std::string_view::npos)return {};type=*part;}
    return EntityCallView{EntityId(value),*method,input,request,type};
}
// Shared route validation for native clients and inter-node entity routing.
inline std::string entity_route(std::string_view type,std::string_view method) {
    for(auto part:{type,method})if(part.empty()||part.size()>127||part.find_first_of(".\0",0,2)!=std::string_view::npos)
        throw std::invalid_argument("invalid entity route");
    std::string out;out.reserve(type.size()+1+method.size());out.append(type);out+='.';out.append(method);return out;
}
// Entity RPC wire envelope: FSE1, big-endian uint16 ID length, ID, opaque payload.
inline std::string entity_message(std::string_view id, std::string_view payload, std::string_view request_id = {}) {
    if (id.empty() || id.size() > 256) throw std::invalid_argument("invalid entity ID");
    if (request_id.size() > 128) throw std::invalid_argument("request ID too large");
    std::string result = request_id.empty() ? "FSE1" : "FSE2";
    result.reserve(6 + id.size() + payload.size() + (request_id.empty() ? 0 : 2 + request_id.size()));
    result.push_back(static_cast<char>(id.size() >> 8));
    result.push_back(static_cast<char>(id.size() & 255));
    result.append(id);
    if (!request_id.empty()) { result.push_back(static_cast<char>(request_id.size() >> 8)); result.push_back(static_cast<char>(request_id.size())); result.append(request_id); }
    result.append(payload);
    return result;
}
struct EntityMessage { std::string id, payload, request_id; };
// Borrowed synchronous parse. The input buffer must outlive every returned view.
struct EntityMessageView { std::string_view id, payload, request_id; };
namespace entity_message_detail {
template<class Message> inline std::optional<Message> parse_entity(std::string_view input) {
    if (input.size() < 6 || (input.substr(0, 4) != "FSE1" && input.substr(0, 4) != "FSE2")) return std::nullopt;
    auto length = (static_cast<unsigned char>(input[4]) << 8) | static_cast<unsigned char>(input[5]);
    if (!length || length > 256 || input.size() < 6u + length) return std::nullopt;
    using Text=decltype(Message::request_id);
    auto payload = input.substr(6 + length); Text token;
    if (input.substr(0, 4) == "FSE2") {
        if (payload.size() < 2) return std::nullopt;
        auto size = (static_cast<unsigned char>(payload[0]) << 8) | static_cast<unsigned char>(payload[1]);
        if (!size || size > 128 || payload.size() < 2u + size) return std::nullopt;
        token = payload.substr(2, size); payload.remove_prefix(2 + size);
    }
    return Message{Text(input.substr(6,length)),Text(payload),std::move(token)};
}
} // namespace entity_message_detail
inline std::optional<EntityMessageView> parse_entity_message_view(std::string_view input) {
    return entity_message_detail::parse_entity<EntityMessageView>(input);
}
inline std::optional<EntityMessage> parse_entity_message(std::string_view input) {
    return entity_message_detail::parse_entity<EntityMessage>(input);
}
// Internal state-machine boundary, never accepted as client-supplied state.
inline std::string state_message(std::string_view state, std::string_view payload) {
    if (state.size() > UINT32_MAX) throw std::length_error("state too large");
    std::string result = "FSS1";
    result.reserve(8 + state.size() + payload.size());
    for (int shift = 24; shift >= 0; shift -= 8) result.push_back(static_cast<char>(state.size() >> shift));
    result.append(state); result.append(payload); return result;
}
struct StateMessage { std::string state, payload; };
struct StateMessageView { std::string_view state, payload; };
namespace entity_message_detail {
template<class Message> inline std::optional<Message> parse_state(std::string_view input) {
    if (input.size() < 8 || input.substr(0, 4) != "FSS1") return std::nullopt;
    std::uint32_t length = 0;
    for (int i = 4; i < 8; ++i) length = (length << 8) | static_cast<unsigned char>(input[i]);
    if (length > input.size() - 8) return std::nullopt;
    using Text=decltype(Message::state);
    return Message{Text(input.substr(8,length)),Text(input.substr(8+length))};
}
} // namespace entity_message_detail
inline std::optional<StateMessageView> parse_state_message_view(std::string_view input) {
    return entity_message_detail::parse_state<StateMessageView>(input);
}
inline std::optional<StateMessage> parse_state_message(std::string_view input) {
    return entity_message_detail::parse_state<StateMessage>(input);
}
}
