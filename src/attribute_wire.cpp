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
bool field_paths(const AttributeUpdate& update) {
    for(const auto& patch:update.patches) if(patch.replace || !patch.path.empty()) return true;
    return false;
}
void check_key(const std::string& key) {
    if(key.empty() || key.size()>127 || key.find('\0')!=std::string::npos) throw std::invalid_argument("invalid patch key");
}
template<class Output> void encode_patch(Output& out,const AttributePatch& patch,bool paths,std::size_t& remaining) {
    check_name(patch.property);
    put(out,patch.property.size(),1); out+=patch.property;
    if(patch.path.size()>8) throw std::length_error("patch path depth limit");
    if(paths) {
        put(out,patch.path.size(),1);
        for(const auto& part:patch.path) {
            if(auto key=std::get_if<std::string>(&part)) {
                check_key(*key); put(out,1,1); put(out,key->size(),1); out+=*key;
            } else {
                auto index=std::get<std::size_t>(part);
                if(index>=256) throw std::invalid_argument("invalid patch path index");
                put(out,0,1); put(out,index,2);
            }
        }
    }
    put(out,patch.replace?2:patch.map?1:0,1);
    if(patch.replace) {
        if(patch.map || !patch.value || !patch.key.empty() || !patch.insert.empty() || patch.index || patch.remove)
            throw std::invalid_argument("invalid field replacement");
        encode_value(out,*patch.value,static_cast<unsigned>(patch.path.size()),remaining);
    } else if(patch.map) {
        check_key(patch.key);
        if(!patch.insert.empty() || patch.index || patch.remove) throw std::invalid_argument("invalid map patch");
        put(out,patch.key.size(),1); out+=patch.key; put(out,patch.value.has_value(),1);
        if(patch.value) encode_value(out,*patch.value,static_cast<unsigned>(patch.path.size()+1),remaining);
    } else {
        if(patch.index>256 || patch.remove>256 || patch.insert.size()>256 || !patch.key.empty() || patch.value)
            throw std::invalid_argument("invalid sequence patch");
        put(out,patch.index,2); put(out,patch.remove,2); put(out,patch.insert.size(),2);
        for(const auto& child:patch.insert) encode_value(out,child,static_cast<unsigned>(patch.path.size()+1),remaining);
    }
    if(out.size()>limit) throw std::length_error("attribute image too large");
}
template<class Output> void encode_update(Output& out,const AttributeUpdate& update) {
    std::size_t remaining=4096;
    encode_image(out,update.values,update.base,update.revision,update.snapshot,&remaining);
    if(update.patches.empty()) return;
    if(update.snapshot || update.patches.size()>256) throw std::invalid_argument("invalid container patches");
    bool paths=field_paths(update); put(out,update.patches.size(),2);
    for(const auto& patch:update.patches) {
        if(update.values.contains(patch.property)) throw std::invalid_argument("replacement and patch overlap");
        encode_patch(out,patch,paths,remaining);
    }
}
// Validate the merged image without copying unchanged values. Limits apply to
// the accumulated replica too, not just to each individual incoming delta.
std::size_t validate_overlay(const AttributeStruct& current, const AttributeStruct& delta) {
    SizeOnly out; out.bytes=23;
    std::size_t remaining=4096, count=0;
    std::size_t native_bytes=256;
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
        native_bytes+=attribute_entry_bytes(field->first,field->second);
    }
    return out.size()+native_bytes;
}
}
std::string encode_attributes(const AttributeUpdate& update) {
    std::string out;
    encode_update(out,update);
    if(!update.patches.empty()) out[3]=field_paths(update)?'3':'2';
    return out;
}
std::size_t attribute_wire_size(const AttributeUpdate& update) {
    SizeOnly out; encode_update(out,update); return out.size();
}
std::size_t attribute_wire_size(const AttributeStruct& values) {
    SizeOnly out; encode_image(out,values,0,0,true); return out.size();
}
AttributeUpdate decode_attributes(std::string_view in) {
    bool paths=in.starts_with("FSA3");
    bool patched=paths || in.starts_with("FSA2");
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
            if(paths) {
                auto depth=get(in,1); if(depth>8) throw std::length_error("patch path depth limit");
                for(std::uint64_t j=0;j<depth;++j) {
                    auto tag=get(in,1);
                    if(tag==0) {
                        auto index=get(in,2); if(index>=256) throw std::invalid_argument("invalid patch path index");
                        patch.path.emplace_back(static_cast<std::size_t>(index));
                    } else if(tag==1) {
                        n=get(in,1); if(!n || n>127 || n>in.size()) throw std::invalid_argument("invalid path key");
                        std::string key(in.substr(0,n)); in.remove_prefix(n); check_key(key); patch.path.emplace_back(std::move(key));
                    } else throw std::invalid_argument("invalid path component");
                }
            }
            auto kind=get(in,1); if(kind>(paths?2u:1u)) throw std::invalid_argument("invalid patch kind");
            patch.map=kind==1; patch.replace=kind==2;
            if(patch.replace) patch.value=decode_value(in,static_cast<unsigned>(patch.path.size()),remaining);
            else if(patch.map) {
                n=get(in,1); if(!n || n>127 || n>in.size()) throw std::invalid_argument("invalid patch key");
                patch.key=std::string(in.substr(0,n)); in.remove_prefix(n);
                if(patch.key.find('\0')!=std::string::npos) throw std::invalid_argument("invalid patch key");
                auto present=get(in,1); if(present>1) throw std::invalid_argument("invalid patch presence");
                if(present) patch.value=decode_value(in,static_cast<unsigned>(patch.path.size()+1),remaining);
            } else {
                patch.index=get(in,2); patch.remove=get(in,2); n=get(in,2);
                if(patch.index>256 || patch.remove>256 || n>256) throw std::invalid_argument("invalid splice bounds");
                for(std::uint64_t j=0;j<n;++j) patch.insert.push_back(decode_value(in,static_cast<unsigned>(patch.path.size()+1),remaining));
            }
            result.patches.push_back(std::move(patch));
        }
    }
    if (!in.empty()) throw std::invalid_argument("trailing attribute bytes");
    return result;
}
static AttributeUpdate container_delta(const AttributeUpdate& previous,const AttributeUpdate& next) {
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
namespace {
std::size_t patch_cost(const AttributePatch& patch) {
    SizeOnly out; std::size_t remaining=4096; encode_patch(out,patch,true,remaining); return out.size();
}
// At each subtree choose among a replacement, recursive field edits, and a
// sequence splice. Bounded to the existing schema depth/node and patch limits.
std::vector<AttributePatch> diff_fields(const std::string& property,
        std::vector<std::variant<std::string,std::size_t>> path,
        const AttributeValue& before,const AttributeValue& after) {
    if(before==after) return {};
    AttributePatch replacement; replacement.property=property; replacement.path=path;
    replacement.replace=true; replacement.value=after;
    std::vector<AttributePatch> edits;
    auto append=[&](auto part,const AttributeValue& old,const AttributeValue& value) {
        auto child_path=path; child_path.emplace_back(std::move(part));
        auto children=diff_fields(property,std::move(child_path),old,value);
        for(auto& child:children) edits.push_back(std::move(child));
    };
    if(path.size()<8) {
        if(auto old=std::get_if<AttributeStruct>(&before)) {
            if(auto value=std::get_if<AttributeStruct>(&after); value && old->size()==value->size()) {
                bool same_keys=true;
                for(const auto& [key,child]:*old) { (void)child; if(!value->contains(key)) {same_keys=false; break;} }
                if(same_keys) for(const auto& [key,child]:*value) { append(key,old->at(key),child); if(edits.size()>256) break; }
            }
        } else if(auto old=std::get_if<AttributeMap>(&before)) {
            if(auto value=std::get_if<AttributeMap>(&after)) {
                for(const auto& [key,child]:*old) if(!value->contains(key)) {
                    (void)child; AttributePatch edit; edit.property=property; edit.path=path; edit.map=true; edit.key=key; edits.push_back(std::move(edit));
                }
                for(const auto& [key,child]:*value) {
                    if(edits.size()>256) break;
                    if(old->contains(key)) append(key,old->at(key),child);
                    else { AttributePatch edit; edit.property=property; edit.path=path; edit.map=true; edit.key=key; edit.value=child; edits.push_back(std::move(edit)); }
                }
            }
        } else if(auto old=std::get_if<AttributeArray>(&before)) {
            if(auto value=std::get_if<AttributeArray>(&after)) {
                std::size_t prefix=0,suffix=0;
                while(prefix<old->size() && prefix<value->size() && (*old)[prefix]==(*value)[prefix]) ++prefix;
                while(suffix<old->size()-prefix && suffix<value->size()-prefix && (*old)[old->size()-1-suffix]==(*value)[value->size()-1-suffix]) ++suffix;
                AttributePatch splice; splice.property=property; splice.path=path; splice.index=prefix; splice.remove=old->size()-prefix-suffix;
                splice.insert.assign(value->begin()+prefix,value->end()-suffix);
                if(old->size()==value->size()) {
                    for(std::size_t i=prefix;i<value->size()-suffix;++i) { append(i,(*old)[i],(*value)[i]); if(edits.size()>256) break; }
                }
                std::size_t cost=0; for(const auto& edit:edits) cost+=patch_cost(edit);
                if(edits.empty() || edits.size()>256 || patch_cost(splice)<=cost) edits={std::move(splice)};
            }
        }
    }
    std::size_t cost=0; for(const auto& edit:edits) cost+=patch_cost(edit);
    if(edits.empty() || edits.size()>256 || patch_cost(replacement)<=cost) return {std::move(replacement)};
    return edits;
}
}
AttributeUpdate attribute_delta(const AttributeUpdate& previous,const AttributeUpdate& next) {
    auto best=container_delta(previous,next);
    if(best.snapshot) return best;
    AttributeUpdate fields{previous.revision,next.revision,false,{}};
    for(const auto& [name,value]:next.values) {
        auto old=previous.values.find(name);
        if(old==previous.values.end()) { fields.values.emplace(name,value); continue; }
        if(old->second==value) continue;
        auto edits=diff_fields(name,{},old->second,value);
        if(edits.size()==1 && edits.front().replace && edits.front().path.empty()) fields.values.emplace(name,value);
        else for(auto& edit:edits) fields.patches.push_back(std::move(edit));
        if(fields.patches.size()>256) return best;
    }
    if(fields.patches.empty()) return best;
    try { if(attribute_wire_size(fields)<attribute_wire_size(best)) return fields; }
    catch(const std::length_error&) {} // dense edits can exceed record/node bounds
    return best;
}
AttributeUpdate attribute_write_delta(const AttributeStruct& confirmed,std::uint64_t confirmed_revision,
                                     const AttributeStruct& values,std::uint64_t expected_revision) {
    if(expected_revision!=confirmed_revision) return {expected_revision,expected_revision,false,values};
    AttributeUpdate before{0,expected_revision,true,{}},after{0,expected_revision,true,values};
    for(const auto& [key,value]:values) {
        (void)value; auto it=confirmed.find(key);
        if(it!=confirmed.end()) before.values.emplace(key,it->second);
    }
    auto update=attribute_delta(before,after);
    for(const auto& [key,value]:values) {
        auto it=confirmed.find(key);
        if(it!=confirmed.end() && it->second==value) update.values.emplace(key,value);
    }
    // Restoring explicit no-op roots can make a sparse record larger.
    AttributeUpdate full{expected_revision,expected_revision,false,values};
    auto full_size=attribute_wire_size(full);
    try { if(attribute_wire_size(update)<full_size) return update; }
    catch(const std::length_error&) {}
    return full;
}
EntityAttributes::Mutation stage_attribute_proposal(const EntityAttributes& model,const AttributeUpdate& proposal,const CallIdentity& who) {
    (void)attribute_wire_size(proposal);
    if(proposal.snapshot || proposal.base!=model.revision() || proposal.revision!=proposal.base)
        throw std::invalid_argument("invalid or stale client proposal");
    if(proposal.patches.empty()) return model.stage(proposal.values,who);
    // Check all root permissions before resolving any client-supplied path.
    EntityAttributes::Values roots=proposal.values;
    for(const auto& patch:proposal.patches) roots.try_emplace(patch.property,model.all_values().at(patch.property));
    (void)model.stage(roots,who);
    AttributeReplica candidate;
    candidate.apply({0,model.revision(),true,model.all_values()});
    if(!candidate.apply(proposal)) throw std::invalid_argument("unapplicable client proposal");
    for(auto& [name,value]:roots) value=candidate.values().at(name);
    return model.stage(roots,who);
}
bool AttributeReplica::apply(const AttributeUpdate& update) {
    return apply(update,std::numeric_limits<std::size_t>::max());
}
bool AttributeReplica::apply(const AttributeUpdate& update,std::size_t max_image_bytes) {
    // Validate programmatic updates too; never apply partially on validation or allocation failure.
    (void)attribute_wire_size(update);
    if (!update.snapshot && (!initialized_ || update.base != revision_)) return false;
    if (initialized_ && update.revision < revision_) return false;
    if(!update.patches.empty()) {
        AttributeUpdate materialized{update.base,update.revision,false,update.values};
        for(const auto& patch:update.patches) {
            auto current=materialized.values.find(patch.property);
            if(current==materialized.values.end()) current=materialized.values.emplace(patch.property,values().at(patch.property)).first;
            auto* target=&current->second;
            for(const auto& part:patch.path) {
                if(auto key=std::get_if<std::string>(&part)) {
                    if(auto fields=std::get_if<AttributeStruct>(target)) target=&fields->at(*key);
                    else target=&std::get<AttributeMap>(*target).at(*key);
                } else target=&std::get<AttributeArray>(*target).at(std::get<std::size_t>(part));
            }
            if(patch.replace) *target=*patch.value;
            else if(patch.map) {
                auto& entries=std::get<AttributeMap>(*target);
                if(patch.value) entries.insert_or_assign(patch.key,*patch.value);
                else if(!entries.erase(patch.key)) throw std::invalid_argument("missing map patch key");
                if(entries.size()>256) throw std::length_error("map patch size limit");
            } else {
                auto& items=std::get<AttributeArray>(*target);
                if(patch.index>items.size() || patch.remove>items.size()-patch.index || items.size()-patch.remove+patch.insert.size()>256)
                    throw std::invalid_argument("splice outside sequence");
                auto at=items.erase(items.begin()+patch.index,items.begin()+patch.index+patch.remove);
                items.insert(at,patch.insert.begin(),patch.insert.end());
            }
        }
        return apply(materialized,max_image_bytes);
    }
    const auto next_revision=update.revision;
    auto count=update.snapshot?attribute_wire_size(update)+attribute_image_bytes(update.values):validate_overlay(values(),update.values);
    if(count>max_image_bytes)throw AttributeReplicaBudget{};
    if (update.snapshot) {
        auto next=std::make_shared<AttributeUpdate>(AttributeUpdate{0,next_revision,true,update.values});
        image_=std::move(next);
    } else {
        // Allocate before mutation; transfer new map nodes without allocation.
        static_assert(std::is_nothrow_swappable_v<AttributeValue>);
        auto prepared=update.values;
        // Retained snapshots and copied replicas share immutable old versions.
        // Detach only when a reader holds the current version; otherwise mutate
        // the existing carrier after all fallible preparation has succeeded.
        if(image_.use_count()!=1)image_=std::make_shared<AttributeUpdate>(*image_);
        auto& values_=image_->values;
        while (!prepared.empty()) {
            auto field=prepared.begin(); auto existing=values_.find(field->first);
            if (existing==values_.end()) values_.insert(prepared.extract(field));
            else { existing->second.swap(field->second); prepared.erase(field); }
        }
    }
    image_->revision=next_revision; revision_ = next_revision; initialized_ = true; bytes_=count; return true;
}
}
