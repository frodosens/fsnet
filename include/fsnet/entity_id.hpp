#pragma once
#include <cstdint>
#include <compare>
#include <functional>
#include <stdexcept>
#include <string>
#include <string_view>

namespace fsnet {
// Logical identity, shared by the native SDK, routing and storage. Lua uses the
// canonical 16-character lowercase hexadecimal representation, never a float.
class EntityId {
    std::uint64_t value_;
public:
    explicit EntityId(std::uint64_t value) : value_(value) {
        if (!value) throw std::invalid_argument("zero entity identity");
    }
    explicit EntityId(std::string_view text) : value_(0) {
        if (text.size()!=16) throw std::invalid_argument("invalid entity identity");
        for (char c:text) {
            unsigned digit;
            if (c>='0' && c<='9') digit=static_cast<unsigned>(c-'0');
            else if(c>='a' && c<='f') digit=static_cast<unsigned>(c-'a'+10);
            else throw std::invalid_argument("noncanonical entity identity");
            value_=(value_<<4)|digit;
        }
        if (!value_) throw std::invalid_argument("zero entity identity");
    }
    std::uint64_t value() const noexcept { return value_; }
    std::string str() const {
        constexpr char hex[]="0123456789abcdef";
        std::string out(16,'0'); auto value=value_;
        for (int i=15;i>=0;--i) { out[static_cast<std::size_t>(i)]=hex[value&15]; value>>=4; }
        return out;
    }
    friend bool operator==(EntityId,EntityId)=default;
    friend auto operator<=>(EntityId,EntityId)=default;
};
inline bool valid_entity_id(std::string_view text) noexcept {
    if(text.size()!=16 || text=="0000000000000000") return false;
    for(char c:text) if(!((c>='0' && c<='9') || (c>='a' && c<='f'))) return false;
    return true;
}
}
template<> struct std::hash<fsnet::EntityId> {
    std::size_t operator()(fsnet::EntityId id) const noexcept { return std::hash<std::uint64_t>{}(id.value()); }
};
