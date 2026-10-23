#include "fsnet/attribute_wire.hpp"
#include <bit>
#include <cmath>
#include <limits>
#include <type_traits>

namespace fsnet {
namespace {
constexpr std::size_t limit = 1024 * 1024;
struct SizeOnly {
    std::size_t bytes = 0;
    void operator+=(char) { ++bytes; }
    void operator+=(const std::string& value) { bytes += value.size(); }
    std::size_t size() const { return bytes; }
};
template<class Output> void put(Output& out, std::uint64_t n, unsigned count) {
    for (unsigned i = count; i; --i) out += static_cast<char>(n >> ((i - 1) * 8));
}
std::uint64_t get(std::string_view& in, unsigned count) {
    if (in.size() < count) throw std::invalid_argument("truncated attributes");
    std::uint64_t n = 0;
    for (unsigned i = 0; i < count; ++i) n = (n << 8) | static_cast<unsigned char>(in[i]);
    in.remove_prefix(count); return n;
}
void check_name(const std::string& name) {
    if (name.empty() || name.size() > 127 || name.front() == '$' || name.find_first_of(".\0", 0, 2) != std::string::npos)
        throw std::invalid_argument("invalid property name");
}
template<class Output> void encode_value(Output& out,const AttributeValue& value,unsigned depth,std::size_t& remaining) {
    if(depth>8 || !remaining) throw std::length_error("attribute value depth/node limit");
    --remaining; put(out,value.index(),1);
    if(auto n=std::get_if<std::int64_t>(&value)) put(out,std::bit_cast<std::uint64_t>(*n),8);
    else if(auto n=std::get_if<double>(&value)) {
        if(!std::isfinite(*n)) throw std::invalid_argument("non-finite attribute");
        put(out,std::bit_cast<std::uint64_t>(*n),8);
    } else if(auto b=std::get_if<bool>(&value)) put(out,*b,1);
    else if(auto text=std::get_if<std::string>(&value)) {
        if(text->size()>65536) throw std::length_error("property string too large");
        put(out,text->size(),4); out+=*text;
    } else if(auto fields=std::get_if<AttributeStruct>(&value)) {
        if(fields->empty() || fields->size()>64) throw std::length_error("struct field limit");
        put(out,fields->size(),1);
        for(const auto& [name,child]:*fields) {
            check_name(name); put(out,name.size(),1); out+=name;
            encode_value(out,child,depth+1,remaining);
        }
    } else {
        const auto& items=std::get<AttributeArray>(value);
        if(items.empty() || items.size()>256) throw std::length_error("array length limit");
        put(out,items.size(),2);
        for(const auto& child:items) encode_value(out,child,depth+1,remaining);
    }
    if(out.size()>limit) throw std::length_error("attribute image too large");
}
AttributeValue decode_value(std::string_view& in,unsigned depth,std::size_t& remaining) {
    if(depth>8 || !remaining) throw std::length_error("attribute value depth/node limit");
    --remaining; auto tag=get(in,1);
    if(tag==0) return std::bit_cast<std::int64_t>(get(in,8));
    if(tag==1) {
        auto n=std::bit_cast<double>(get(in,8));
        if(!std::isfinite(n)) throw std::invalid_argument("non-finite attribute");
        return n;
    }
    if(tag==2) { auto b=get(in,1); if(b>1) throw std::invalid_argument("invalid boolean"); return bool(b); }
    if(tag==3) {
        auto length=get(in,4);
        if(length>65536 || length>in.size()) throw std::invalid_argument("invalid property string");
        std::string out(in.substr(0,length)); in.remove_prefix(length); return out;
    }
    if(tag==4) {
        auto count=get(in,1); if(!count || count>64) throw std::invalid_argument("invalid struct field count");
        AttributeStruct out;
        for(std::uint64_t i=0;i<count;++i) {
            auto n=get(in,1); if(n>in.size()) throw std::invalid_argument("truncated struct field");
            std::string name(in.substr(0,n)); in.remove_prefix(n); check_name(name);
            auto child=decode_value(in,depth+1,remaining);
            if(!out.emplace(std::move(name),std::move(child)).second) throw std::invalid_argument("duplicate struct field");
        }
        return out;
    }
    if(tag==5) {
        auto count=get(in,2); if(!count || count>256) throw std::invalid_argument("invalid array length");
        AttributeArray out; out.reserve(count);
        for(std::uint64_t i=0;i<count;++i) out.push_back(decode_value(in,depth+1,remaining));
        return out;
    }
    throw std::invalid_argument("invalid property type");
}
}
namespace {
template<class Output> void encode_image(Output& out, const AttributeStruct& values,
                                        std::uint64_t base, std::uint64_t revision, bool snapshot) {
    if (values.size() > 256 || base > revision || (snapshot && base))
        throw std::invalid_argument("invalid attribute revision or count");
    out += std::string("FSA1"); put(out, snapshot, 1); put(out, base, 8); put(out, revision, 8);
    put(out, values.size(), 2);
    std::size_t remaining=4096;
    for (const auto& [name, value] : values) {
        check_name(name); put(out,name.size(),1); out+=name;
        encode_value(out,value,0,remaining);
        if (out.size() > limit) throw std::length_error("attribute image too large");
    }
}
// Validate the merged image without copying unchanged values. Limits apply to
// the accumulated replica too, not just to each individual incoming delta.
void validate_overlay(const AttributeStruct& current, const AttributeStruct& delta) {
    SizeOnly out; out.bytes=23;
    std::size_t remaining=4096, count=0;
    auto old=current.begin(), changed=delta.begin();
    while (old!=current.end() || changed!=delta.end()) {
        const AttributeStruct::value_type* field;
        if (changed==delta.end() || (old!=current.end() && old->first<changed->first)) field=&*old++;
        else {
            if (old!=current.end() && old->first==changed->first) ++old;
            field=&*changed++;
        }
        if (++count>256) throw std::invalid_argument("invalid attribute revision or count");
        check_name(field->first); put(out,field->first.size(),1); out+=field->first;
        encode_value(out,field->second,0,remaining);
    }
}
}
std::string encode_attributes(const AttributeUpdate& update) {
    std::string out;
    encode_image(out,update.values,update.base,update.revision,update.snapshot);
    return out;
}
std::size_t attribute_wire_size(const AttributeUpdate& update) {
    SizeOnly out; encode_image(out,update.values,update.base,update.revision,update.snapshot); return out.size();
}
std::size_t attribute_wire_size(const AttributeStruct& values) {
    SizeOnly out; encode_image(out,values,0,0,true); return out.size();
}
AttributeUpdate decode_attributes(std::string_view in) {
    if (in.size() > limit || !in.starts_with("FSA1")) throw std::invalid_argument("invalid attribute record");
    in.remove_prefix(4); auto flag = get(in, 1);
    if (flag > 1) throw std::invalid_argument("invalid snapshot flag");
    AttributeUpdate result; result.snapshot = flag; result.base = get(in, 8); result.revision = get(in, 8);
    auto count = get(in, 2);
    if (count > 256 || result.base > result.revision || (result.snapshot && result.base)) throw std::invalid_argument("invalid attribute header");
    std::size_t remaining=4096;
    for (std::uint64_t i = 0; i < count; ++i) {
        auto size = get(in, 1);
        if (size > in.size()) throw std::invalid_argument("truncated attribute name");
        std::string name(in.substr(0, size)); in.remove_prefix(size); check_name(name);
        auto value=decode_value(in,0,remaining);
        if (!result.values.emplace(std::move(name), std::move(value)).second) throw std::invalid_argument("duplicate property");
    }
    if (!in.empty()) throw std::invalid_argument("trailing attribute bytes");
    return result;
}
bool AttributeReplica::apply(const AttributeUpdate& update) {
    // Validate programmatic updates too; never apply partially on validation or allocation failure.
    (void)attribute_wire_size(update);
    if (!update.snapshot && (!initialized_ || update.base != revision_)) return false;
    if (initialized_ && update.revision < revision_) return false;
    if (update.snapshot) {
        auto next=update.values; values_.swap(next);
    } else {
        validate_overlay(values_,update.values);
        // Allocate before mutation; transfer new map nodes without allocation.
        static_assert(std::is_nothrow_swappable_v<AttributeValue>);
        auto prepared=update.values;
        while (!prepared.empty()) {
            auto field=prepared.begin(); auto existing=values_.find(field->first);
            if (existing==values_.end()) values_.insert(prepared.extract(field));
            else { existing->second.swap(field->second); prepared.erase(field); }
        }
    }
    revision_ = update.revision; initialized_ = true; return true;
}
}
