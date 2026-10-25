#pragma once
#include <cstdint>
#include <map>
#include <optional>
#include <memory>
#include <limits>
#include <set>
#include <stdexcept>
#include <string>
#include <variant>
#include <vector>
#include <functional>
#include "fsnet/connection.hpp"

namespace fsnet {
enum class Caller { client, server };
struct CallIdentity {
    Caller caller = Caller::client;
    std::string subject;
    std::uint64_t peer = 0; // transport-local connection; never serialized as identity
    ConnectionRef connection{};
    std::function<bool()> current{}; // owner-thread revalidation, never serialized
    bool live()const {try{return !current || current();}catch(...){return false;}}
    bool owns(const std::string& owner)const {
        return live() && (owner.starts_with("connection/") ? (connection && connection.key()==owner) : (!subject.empty() && subject==owner));
    }
};
enum class Access { none, read, write, read_write };
enum class Visibility { none, owner, observers };
struct RpcPolicy {
    bool client = false, server = false, owner_only = false;
    bool allows(const CallIdentity& identity, const std::string& owner) const {
        return identity.live() && (identity.caller == Caller::server ? server : client) &&
            (!owner_only || identity.owns(owner));
    }
};
struct AttributeValue;
using AttributeStruct = std::map<std::string, AttributeValue>;
using AttributeArray = std::vector<AttributeValue>;
struct AttributeMap : AttributeStruct { using AttributeStruct::map; };
// Owned recursive values: copies cannot alias another entity or a Lua table.
struct AttributeValue : std::variant<std::int64_t,double,bool,std::string,AttributeStruct,AttributeArray,AttributeMap> {
    using variant::variant;
    using variant::operator=;
};
std::size_t attribute_image_bytes(const AttributeStruct&); // conservative native value allocation charge
std::size_t attribute_entry_bytes(std::string_view,const AttributeValue&); // one root, without copying its value
enum class AttributeType { integer, number, boolean, string, structure, array, list, map, set };
struct AttributeDefinition {
    AttributeType type;
    AttributeValue initial;
    Access server = Access::read_write, client = Access::read;
    Visibility visibility = Visibility::owner;
    bool persist = true;
    std::int64_t minimum = std::numeric_limits<std::int64_t>::min(), maximum = std::numeric_limits<std::int64_t>::max();
    bool driver_authority = false; // local simulation may write; network authority is unchanged
    std::map<std::string, AttributeDefinition> fields{};
    std::shared_ptr<const AttributeDefinition> element{};
    std::size_t length = 0;
    std::size_t max_items = 0; // dynamic containers: 1..256, empty is allowed
};
struct EntitySchema {
    std::map<std::string, AttributeDefinition> attributes;
    std::map<std::string, RpcPolicy> methods;
    bool owner_from_id = false;
    void validate() const;
};
struct AttributeView {
    std::string subject;
    bool observer = false;
    ConnectionRef connection{};
};
// Bounded edits below a top-level property. Paths contain exact string keys
// or zero-based sequence indices; replacements address an existing value.
struct AttributePatch {
    std::string property;
    bool map = false;
    std::string key;
    std::optional<AttributeValue> value; // map replacement; absent means erase
    std::size_t index = 0, remove = 0;
    AttributeArray insert;
    std::vector<std::variant<std::string,std::size_t>> path{};
    bool replace = false;
};
struct AttributeUpdate {
    std::uint64_t base = 0, revision = 0;
    bool snapshot = false;
    std::map<std::string, AttributeValue> values;
    std::vector<AttributePatch> patches{};
};
// Owner-thread carrier. A mutation is staged separately; only publish after the
// enclosing entity CAS succeeds. No callbacks, transport or storage under this API.
class EntityAttributes {
public:
    using Values = std::map<std::string, AttributeValue>;
    struct Mutation { std::uint64_t expected; Values values; };
    explicit EntityAttributes(EntitySchema schema, std::string owner);
    const AttributeValue& read(const std::string&, const CallIdentity&) const;
    Mutation stage(const Values&, const CallIdentity&) const;
    // Returns false for a stale mutation. Throws for invalid values, never applies
    // a partial update. Call only after the enclosing durable commit succeeds.
    bool publish(const Mutation&);
    AttributeUpdate snapshot(const AttributeView&) const;
    // A full filtered snapshot is returned when history cannot bridge `since`.
    AttributeUpdate changes(std::uint64_t since, const AttributeView&) const;
    Values persistent() const;
    void restore(const Values&, std::uint64_t revision);
    void restore_full(const Values&, std::uint64_t revision);
    const Values& all_values() const { return values_; }
    std::uint64_t revision() const { return revision_; }
    const EntitySchema& schema() const { return schema_; }
private:
    EntitySchema schema_;
    std::string owner_;
    Values values_;
    std::uint64_t revision_ = 0;
    std::map<std::string, std::uint64_t> modified_;
    bool visible(const AttributeDefinition&, const AttributeView&) const;
};
}
