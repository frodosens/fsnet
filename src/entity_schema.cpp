#include "fsnet/entity_schema.hpp"
#include <cmath>
#include <limits>

namespace fsnet {
namespace {
bool reads(Access a) { return a == Access::read || a == Access::read_write; }
bool writes(Access a) { return a == Access::write || a == Access::read_write; }
bool valid(const AttributeDefinition& d, const AttributeValue& v) {
    if (v.index() != static_cast<std::size_t>(d.type)) return false;
    if (auto n = std::get_if<std::int64_t>(&v)) return *n >= d.minimum && *n <= d.maximum;
    if (auto n = std::get_if<double>(&v)) return std::isfinite(*n);
    if (auto s = std::get_if<std::string>(&v)) return s->size() <= 64 * 1024;
    return true;
}
bool name_valid(const std::string& s) {
    return !s.empty() && s.size() <= 127 && s.find_first_of(".\0", 0, 2) == std::string::npos && s.front() != '$';
}
}
void EntitySchema::validate() const {
    if (attributes.size() > 256 || methods.size() > 256) throw std::invalid_argument("entity schema limit");
    for (const auto& [name, d] : attributes) {
        if (!name_valid(name) || d.minimum > d.maximum || !valid(d, d.initial)) throw std::invalid_argument("invalid attribute definition");
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
        (who.caller == Caller::client && !visible(d, {who.subject, false}))) throw std::runtime_error("attribute read denied");
    return values_.at(name);
}
EntityAttributes::Mutation EntityAttributes::stage(const Values& values, const CallIdentity& who) const {
    if (values.size() > schema_.attributes.size()) throw std::invalid_argument("attribute update limit");
    for (const auto& [name, value] : values) {
        const auto& d = schema_.attributes.at(name);
        if (!writes(who.caller == Caller::server ? d.server : d.client) ||
            (who.caller == Caller::client && (who.subject.empty() || who.subject != owner_)))
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
    // Prepare all allocating work before swapping the committed image.
    auto next = values_; auto modified = modified_; bool changed = false;
    for (const auto& [name, value] : mutation.values) if (next.at(name) != value) {
        next.at(name) = value; modified.at(name) = revision_ + 1; changed = true;
    }
    if (changed) { values_.swap(next); modified_.swap(modified); ++revision_; }
    return true;
}
bool EntityAttributes::visible(const AttributeDefinition& d, const AttributeView& view) const {
    if (!reads(d.client)) return false;
    if (d.visibility == Visibility::owner) return !view.subject.empty() && view.subject == owner_;
    return d.visibility == Visibility::observers && (view.observer || (!view.subject.empty() && view.subject == owner_));
}
AttributeUpdate EntityAttributes::snapshot(const AttributeView& view) const {
    AttributeUpdate update{0, revision_, true, {}};
    for (const auto& [name, d] : schema_.attributes) if (visible(d, view)) update.values.emplace(name, values_.at(name));
    return update;
}
AttributeUpdate EntityAttributes::changes(std::uint64_t since, const AttributeView& view) const {
    if (since > revision_) return snapshot(view);
    AttributeUpdate update{since, revision_, false, {}};
    for (const auto& [name, d] : schema_.attributes)
        if (visible(d, view) && modified_.at(name) > since) update.values.emplace(name, values_.at(name));
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
