#include "fsnet/entity_schema.hpp"
#include <cmath>
#include <limits>
#include <type_traits>

namespace fsnet {
namespace {
bool reads(Access a) { return a == Access::read || a == Access::read_write; }
bool writes(Access a) { return a == Access::write || a == Access::read_write; }
bool valid(const AttributeDefinition& d, const AttributeValue& v,unsigned depth=0,std::size_t* remaining=nullptr) {
    std::size_t budget=4096; if(!remaining) remaining=&budget;
    auto expected=d.type==AttributeType::list || d.type==AttributeType::set ? 5u :
        d.type==AttributeType::map ? 6u : static_cast<unsigned>(d.type);
    if(depth>8 || !*remaining || v.index()!=expected) return false;
    --*remaining;
    if(auto n=std::get_if<std::int64_t>(&v)) return *n>=d.minimum && *n<=d.maximum;
    if(auto n=std::get_if<double>(&v)) return std::isfinite(*n);
    if(auto text=std::get_if<std::string>(&v)) return text->size()<=65536;
    if(auto fields=std::get_if<AttributeStruct>(&v)) {
        if(fields->size()!=d.fields.size()) return false;
        for(const auto& [name,child]:*fields) {
            auto it=d.fields.find(name);
            if(it==d.fields.end() || !valid(it->second,child,depth+1,remaining)) return false;
        }
    }
    if(auto items=std::get_if<AttributeArray>(&v)) {
        if(!d.element || (d.type==AttributeType::array ? items->size()!=d.length : items->size()>d.max_items)) return false;
        if(d.type==AttributeType::set) for(std::size_t i=0;i<items->size();++i)
            for(std::size_t j=0;j<i;++j) if((*items)[i]==(*items)[j]) return false;
        for(const auto& child:*items) if(!valid(*d.element,child,depth+1,remaining)) return false;
    }
    if(auto entries=std::get_if<AttributeMap>(&v)) {
        if(!d.element || entries->size()>d.max_items) return false;
        for(const auto& [key,child]:*entries)
            if(key.empty() || key.size()>127 || key.find('\0')!=std::string::npos || !valid(*d.element,child,depth+1,remaining)) return false;
    }
    return true;
}
bool name_valid(const std::string& s) {
    return !s.empty() && s.size() <= 127 && s.find_first_of(".\0", 0, 2) == std::string::npos && s.front() != '$';
}
void definition_valid(const AttributeDefinition& d,unsigned depth,std::size_t& remaining) {
    if(depth>8 || !remaining) throw std::invalid_argument("attribute schema depth/node limit");
    --remaining;
    if(d.minimum>d.maximum) throw std::invalid_argument("invalid attribute range");
    if(d.max_items && d.type!=AttributeType::list && d.type!=AttributeType::map && d.type!=AttributeType::set) throw std::invalid_argument("capacity on non-container");
    if(d.type==AttributeType::structure) {
        if(d.fields.empty() || d.fields.size()>64 || d.element || d.length) throw std::invalid_argument("invalid struct schema");
        for(const auto& [name,child]:d.fields) {
            if(!name_valid(name)) throw std::invalid_argument("invalid struct field");
            definition_valid(child,depth+1,remaining);
        }
    } else if(d.type==AttributeType::array) {
        if(!d.element || !d.fields.empty() || !d.length || d.length>256) throw std::invalid_argument("invalid fixed array schema");
        definition_valid(*d.element,depth+1,remaining);
    } else if(d.type==AttributeType::list || d.type==AttributeType::map || d.type==AttributeType::set) {
        if(!d.element || !d.fields.empty() || d.length || !d.max_items || d.max_items>256) throw std::invalid_argument("invalid container schema");
        if(d.type==AttributeType::set && static_cast<unsigned>(d.element->type)>3) throw std::invalid_argument("set elements must be scalar");
        definition_valid(*d.element,depth+1,remaining);
    } else if(!d.fields.empty() || d.element || d.length || static_cast<unsigned>(d.type)>8)
        throw std::invalid_argument("invalid scalar schema");
}
}
std::size_t attribute_image_bytes(const AttributeStruct& values) {
    std::size_t bytes=256;
    for(const auto& [name,value]:values) bytes+=attribute_entry_bytes(name,value);
    return bytes;
}
std::size_t attribute_entry_bytes(std::string_view name,const AttributeValue& value) {
        std::size_t bytes=256+name.size();
        if(auto text=std::get_if<std::string>(&value)) bytes+=text->size();
        else if(auto fields=std::get_if<AttributeStruct>(&value)) bytes+=attribute_image_bytes(*fields);
        else if(auto entries=std::get_if<AttributeMap>(&value)) bytes+=attribute_image_bytes(*entries);
        else if(auto items=std::get_if<AttributeArray>(&value)) {
            // Reuse the same recursive charge without allocating a map.
            auto charge=[&](auto&& recurse,const AttributeValue& child)->std::size_t {
                if(auto s=std::get_if<std::string>(&child)) return 256+s->size();
                if(auto f=std::get_if<AttributeStruct>(&child)) return attribute_image_bytes(*f);
                if(auto m=std::get_if<AttributeMap>(&child)) return attribute_image_bytes(*m);
                if(auto a=std::get_if<AttributeArray>(&child)) { std::size_t n=256; for(const auto& v:*a) n+=recurse(recurse,v); return n; }
                return 256;
            };
            for(const auto& child:*items) bytes+=charge(charge,child);
        }
    return bytes;
}
void EntitySchema::validate() const {
    if (attributes.size() > 256 || methods.size() > 256) throw std::invalid_argument("entity schema limit");
    std::size_t remaining=4096,values_remaining=4096;
    for (const auto& [name, d] : attributes) {
        definition_valid(d,0,remaining);
        if (!name_valid(name) || d.minimum > d.maximum || !valid(d, d.initial,0,&values_remaining)) throw std::invalid_argument("invalid attribute definition");
        if (d.visibility != Visibility::none && !reads(d.client)) throw std::invalid_argument("cannot synchronize unreadable attribute");
    }
    for (const auto& [name, p] : methods)
        if (!name_valid(name) || (!p.client && !p.server)) throw std::invalid_argument("RPC callers must be explicit");
}
EntityAttributes::EntityAttributes(EntitySchema schema, std::string owner) : schema_(std::move(schema)), owner_(std::move(owner)) {
    schema_.validate();
    for (const auto& [name, d] : schema_.attributes) { values_.emplace(name, d.initial); modified_.emplace(name, 0); }
}
const AttributeValue& EntityAttributes::read(const std::string& name, const CallIdentity& who) const {
    const auto& d = schema_.attributes.at(name);
    if (!reads(who.caller == Caller::server ? d.server : d.client) ||
        (who.caller==Caller::server?!who.live():!who.owns(owner_)) ||
        (who.caller==Caller::client && d.visibility==Visibility::none)) throw std::runtime_error("attribute read denied");
    return values_.at(name);
}
EntityAttributes::Mutation EntityAttributes::stage(const Values& values, const CallIdentity& who) const {
    if (values.size() > schema_.attributes.size()) throw std::invalid_argument("attribute update limit");
    if(who.caller==Caller::server?!who.live():!who.owns(owner_))throw std::runtime_error("attribute write denied");
    for (const auto& [name, value] : values) {
        const auto& d = schema_.attributes.at(name);
        if (!writes(who.caller == Caller::server ? d.server : d.client))
            throw std::runtime_error("attribute write denied");
        if (!valid(d, value)) throw std::invalid_argument("attribute type or size mismatch");
    }
    return {revision_, values};
}
bool EntityAttributes::publish(const Mutation& mutation) {
    if (mutation.expected != revision_) return false;
    if (revision_ == std::numeric_limits<std::uint64_t>::max()) throw std::overflow_error("attribute revision exhausted");
    for (const auto& [name, value] : mutation.values)
        if (!valid(schema_.attributes.at(name), value)) throw std::invalid_argument("invalid committed attribute");
    // Copy only changed properties. Complete validation/allocation before the
    // first commit; variant swaps and revision assignments cannot throw.
    static_assert(std::is_nothrow_swappable_v<AttributeValue>);
    Values prepared;
    for (const auto& [name, value] : mutation.values)
        if (values_.at(name) != value) prepared.emplace(name, value);
    if (!prepared.empty()) {
        for (auto& [name, value] : prepared) {
            values_.at(name).swap(value); modified_.at(name) = revision_ + 1;
        }
        ++revision_;
    }
    return true;
}
bool EntityAttributes::owned(const AttributeView& view)const {
    return owner_.starts_with("connection/")?view.connection.matches_key(owner_):(!view.subject.empty() && view.subject==owner_);
}
bool EntityAttributes::visible(const AttributeDefinition& d, bool owned, bool observer) {
    if (!reads(d.client)) return false;
    if (d.visibility == Visibility::owner) return owned;
    return d.visibility == Visibility::observers && (observer || owned);
}
AttributeUpdate EntityAttributes::snapshot(const AttributeView& view) const {
    AttributeUpdate update{0, revision_, true, {}};
    const auto is_owned=owned(view);
    for (const auto& [name, d] : schema_.attributes) if (visible(d, is_owned,view.observer)) update.values.emplace(name, values_.at(name));
    return update;
}
AttributeUpdate EntityAttributes::changes(std::uint64_t since, const AttributeView& view) const {
    if (since > revision_) return snapshot(view);
    AttributeUpdate update{since, revision_, false, {}};
    const auto is_owned=owned(view);
    for (const auto& [name, d] : schema_.attributes)
        if (visible(d, is_owned,view.observer) && modified_.at(name) > since) update.values.emplace(name, values_.at(name));
    return update;
}
EntityAttributes::Values EntityAttributes::persistent() const {
    Values result;
    for (const auto& [name, d] : schema_.attributes) if (d.persist) result.emplace(name, values_.at(name));
    return result;
}
void EntityAttributes::restore_full(const Values& values, std::uint64_t revision) {
    auto next = values_;
    for (const auto& [name, value] : values) {
        if (!valid(schema_.attributes.at(name), value)) throw std::invalid_argument("invalid attribute image");
        next.at(name) = value;
    }
    auto modified = modified_;
    for (auto& [name, version] : modified) { (void)name; version = revision; }
    values_.swap(next); modified_.swap(modified); revision_ = revision;
}
void EntityAttributes::restore(const Values& values, std::uint64_t revision) {
    auto next = values_;
    for (const auto& [name, value] : values) {
        const auto& d = schema_.attributes.at(name);
        if (!d.persist || !valid(d, value)) throw std::invalid_argument("invalid persistent attribute");
        next.at(name) = value;
    }
    auto modified = modified_;
    for (auto& [name, version] : modified) { (void)name; version = revision; }
    values_.swap(next); modified_.swap(modified); revision_ = revision;
}
}
