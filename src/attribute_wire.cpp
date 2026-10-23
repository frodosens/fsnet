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
    } else if(auto entries=std::get_if<AttributeMap>(&value)) {
        if(entries->size()>256) throw std::length_error("map entry limit");
        put(out,entries->size(),2);
        for(const auto& [key,child]:*entries) {
            if(key.empty() || key.size()>127 || key.find('\0')!=std::string::npos) throw std::invalid_argument("invalid map key");
            put(out,key.size(),1); out+=key; encode_value(out,child,depth+1,remaining);
        }
    } else {
        const auto& items=std::get<AttributeArray>(value);
        if(items.size()>256) throw std::length_error("array length limit");
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
        auto count=get(in,2); if(count>256) throw std::invalid_argument("invalid array length");
        AttributeArray out; out.reserve(count);
        for(std::uint64_t i=0;i<count;++i) out.push_back(decode_value(in,depth+1,remaining));
        return out;
    }
    if(tag==6) {
        auto count=get(in,2); if(count>256) throw std::invalid_argument("invalid map length");
        AttributeMap out;
        for(std::uint64_t i=0;i<count;++i) {
            auto n=get(in,1); if(!n || n>127 || n>in.size()) throw std::invalid_argument("invalid map key");
            std::string key(in.substr(0,n)); in.remove_prefix(n);
            if(key.find('\0')!=std::string::npos) throw std::invalid_argument("invalid map key");
            auto child=decode_value(in,depth+1,remaining);
            if(!out.emplace(std::move(key),std::move(child)).second) throw std::invalid_argument("duplicate map key");
        }
        return out;
    }
    throw std::invalid_argument("invalid property type");
}
}
namespace {
template<class Output> void encode_image(Output& out, const AttributeStruct& values,
                                        std::uint64_t base, std::uint64_t revision, bool snapshot, std::size_t* shared_budget=nullptr) {
    if (values.size() > 256 || base > revision || (snapshot && base))
        throw std::invalid_argument("invalid attribute revision or count");
    out += std::string("FSA1"); put(out, snapshot, 1); put(out, base, 8); put(out, revision, 8);
    put(out, values.size(), 2);
    std::size_t budget=4096; auto& remaining=shared_budget?*shared_budget:budget;
    for (const auto& [name, value] : values) {
        check_name(name); put(out,name.size(),1); out+=name;
        encode_value(out,value,0,remaining);
        if (out.size() > limit) throw std::length_error("attribute image too large");
    }
}
template<class Output> void encode_update(Output& out,const AttributeUpdate& update) {
    std::size_t remaining=4096;
    encode_image(out,update.values,update.base,update.revision,update.snapshot,&remaining);
    if(update.patches.empty()) return;
    if(update.snapshot || update.patches.size()>256) throw std::invalid_argument("invalid container patches");
    // FSA2 has the FSA1 layout followed by a bounded edit list.
    put(out,update.patches.size(),2);
    for(const auto& patch:update.patches) {
        check_name(patch.property);
        if(update.values.contains(patch.property)) throw std::invalid_argument("replacement and patch overlap");
        put(out,patch.property.size(),1); out+=patch.property;
        put(out,patch.map?1:0,1);
        if(patch.map) {
            if(patch.key.empty() || patch.key.size()>127 || patch.key.find('\0')!=std::string::npos || !patch.insert.empty() || patch.index || patch.remove)
                throw std::invalid_argument("invalid map patch");
            put(out,patch.key.size(),1); out+=patch.key; put(out,patch.value.has_value(),1);
            if(patch.value) encode_value(out,*patch.value,1,remaining);
        } else {
            if(patch.index>256 || patch.remove>256 || patch.insert.size()>256 || !patch.key.empty() || patch.value)
                throw std::invalid_argument("invalid sequence patch");
            put(out,patch.index,2); put(out,patch.remove,2); put(out,patch.insert.size(),2);
            for(const auto& child:patch.insert) encode_value(out,child,1,remaining);
        }
        if(out.size()>limit) throw std::length_error("attribute image too large");
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
    encode_update(out,update);
    if(!update.patches.empty()) out[3]='2';
    return out;
}
std::size_t attribute_wire_size(const AttributeUpdate& update) {
    SizeOnly out; encode_update(out,update); return out.size();
}
std::size_t attribute_wire_size(const AttributeStruct& values) {
    SizeOnly out; encode_image(out,values,0,0,true); return out.size();
}
AttributeUpdate decode_attributes(std::string_view in) {
    bool patched=in.starts_with("FSA2");
    if (in.size() > limit || (!patched && !in.starts_with("FSA1"))) throw std::invalid_argument("invalid attribute record");
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
    if(patched) {
        if(result.snapshot) throw std::invalid_argument("patch snapshot forbidden");
        auto patches=get(in,2); if(!patches || patches>256) throw std::invalid_argument("invalid patch count");
        for(std::uint64_t i=0;i<patches;++i) {
            AttributePatch patch;
            auto n=get(in,1); if(n>in.size()) throw std::invalid_argument("truncated patch property");
            patch.property=std::string(in.substr(0,n)); in.remove_prefix(n); check_name(patch.property);
            if(result.values.contains(patch.property)) throw std::invalid_argument("replacement and patch overlap");
            auto kind=get(in,1); if(kind>1) throw std::invalid_argument("invalid patch kind"); patch.map=kind;
            if(patch.map) {
                n=get(in,1); if(!n || n>127 || n>in.size()) throw std::invalid_argument("invalid patch key");
                patch.key=std::string(in.substr(0,n)); in.remove_prefix(n);
                if(patch.key.find('\0')!=std::string::npos) throw std::invalid_argument("invalid patch key");
                auto present=get(in,1); if(present>1) throw std::invalid_argument("invalid patch presence");
                if(present) patch.value=decode_value(in,1,remaining);
            } else {
                patch.index=get(in,2); patch.remove=get(in,2); n=get(in,2);
                if(patch.index>256 || patch.remove>256 || n>256) throw std::invalid_argument("invalid splice bounds");
                for(std::uint64_t j=0;j<n;++j) patch.insert.push_back(decode_value(in,1,remaining));
            }
            result.patches.push_back(std::move(patch));
        }
    }
    if (!in.empty()) throw std::invalid_argument("trailing attribute bytes");
    return result;
}
AttributeUpdate attribute_delta(const AttributeUpdate& previous,const AttributeUpdate& next) {
    if(!previous.snapshot || !next.snapshot || !previous.patches.empty() || !next.patches.empty()) throw std::invalid_argument("delta requires full images");
    for(const auto& [name,value]:previous.values) { (void)value; if(!next.values.contains(name)) return next; }
    if(next.revision<previous.revision) return next;
    AttributeUpdate full{previous.revision,next.revision,false,{}};
    for(const auto& [name,value]:next.values)
        if(!previous.values.contains(name) || previous.values.at(name)!=value) full.values.emplace(name,value);
    AttributeUpdate sparse{full.base,full.revision,false,{}};
    for(const auto& [name,value]:full.values) {
        auto old=previous.values.find(name); bool edited=false;
        if(old!=previous.values.end()) {
            if(auto before=std::get_if<AttributeArray>(&old->second)) {
                if(auto after=std::get_if<AttributeArray>(&value)) {
                    std::size_t prefix=0,suffix=0;
                    while(prefix<before->size() && prefix<after->size() && (*before)[prefix]==(*after)[prefix]) ++prefix;
                    while(suffix<before->size()-prefix && suffix<after->size()-prefix && (*before)[before->size()-1-suffix]==(*after)[after->size()-1-suffix]) ++suffix;
                    AttributePatch patch; patch.property=name; patch.index=prefix; patch.remove=before->size()-prefix-suffix;
                    patch.insert.assign(after->begin()+prefix,after->end()-suffix);
                    sparse.patches.push_back(std::move(patch)); edited=true;
                }
            } else if(auto before=std::get_if<AttributeMap>(&old->second)) {
                if(auto after=std::get_if<AttributeMap>(&value)) {
                    for(const auto& [key,value]:*before) { (void)value; if(!after->contains(key)) { AttributePatch patch; patch.property=name; patch.map=true; patch.key=key; sparse.patches.push_back(std::move(patch)); } }
                    for(const auto& [key,value]:*after) if(!before->contains(key) || before->at(key)!=value) {
                        AttributePatch patch; patch.property=name; patch.map=true; patch.key=key; patch.value=value; sparse.patches.push_back(std::move(patch));
                    }
                    edited=true;
                }
            }
        }
        if(!edited) sparse.values.emplace(name,value);
    }
    if(sparse.patches.empty() || sparse.patches.size()>256) return full;
    // A single small container may cost less as a replacement. Keep the old
    // representation whenever the sparse form does not save application bytes.
    auto full_size=attribute_wire_size(full);
    try { if(attribute_wire_size(sparse)<full_size) return sparse; }
    catch(const std::length_error&) {} // a dense edit can exceed patch bounds
    return full;
}
bool AttributeReplica::apply(const AttributeUpdate& update) {
    // Validate programmatic updates too; never apply partially on validation or allocation failure.
    (void)attribute_wire_size(update);
    if (!update.snapshot && (!initialized_ || update.base != revision_)) return false;
    if (initialized_ && update.revision < revision_) return false;
    if(!update.patches.empty()) {
        AttributeUpdate materialized{update.base,update.revision,false,update.values};
        for(const auto& patch:update.patches) {
            auto current=materialized.values.find(patch.property);
            if(current==materialized.values.end()) current=materialized.values.emplace(patch.property,values_.at(patch.property)).first;
            if(patch.map) {
                auto& entries=std::get<AttributeMap>(current->second);
                if(patch.value) entries.insert_or_assign(patch.key,*patch.value);
                else if(!entries.erase(patch.key)) throw std::invalid_argument("missing map patch key");
                if(entries.size()>256) throw std::length_error("map patch size limit");
            } else {
                auto& items=std::get<AttributeArray>(current->second);
                if(patch.index>items.size() || patch.remove>items.size()-patch.index || items.size()-patch.remove+patch.insert.size()>256)
                    throw std::invalid_argument("splice outside sequence");
                auto at=items.erase(items.begin()+patch.index,items.begin()+patch.index+patch.remove);
                items.insert(at,patch.insert.begin(),patch.insert.end());
            }
        }
        return apply(materialized);
    }
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
