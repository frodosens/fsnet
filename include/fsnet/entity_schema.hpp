#pragma once
#include <cstdint>
#include <map>
#include <limits>
#include <set>
#include <stdexcept>
#include <string>
#include <variant>
#include <vector>

namespace fsnet {
enum class Caller { client, server };
struct CallIdentity {
    Caller caller = Caller::client;
    std::string subject;
    std::uint64_t peer = 0; // transport-local connection; never serialized as identity
};
enum class Access { none, read, write, read_write };
enum class Visibility { none, owner, observers };
struct RpcPolicy {
    bool client = false, server = false, owner_only = false;
    bool allows(const CallIdentity& identity, const std::string& owner) const {
        return (identity.caller == Caller::server ? server : client) &&
            (!owner_only || (!identity.subject.empty() && identity.subject == owner));
    }
};
using AttributeValue = std::variant<std::int64_t, double, bool, std::string>;
enum class AttributeType { integer, number, boolean, string };
struct AttributeDefinition {
    AttributeType type;
    AttributeValue initial;
    Access server = Access::read_write, client = Access::read;
    Visibility visibility = Visibility::owner;
    bool persist = true;
    std::int64_t minimum = std::numeric_limits<std::int64_t>::min(), maximum = std::numeric_limits<std::int64_t>::max();
};
struct EntitySchema {
    std::map<std::string, AttributeDefinition> attributes;
    std::map<std::string, RpcPolicy> methods;
    void validate() const;
};
struct AttributeView {
    std::string subject;
    bool observer = false;
};
struct AttributeUpdate {
    std::uint64_t base = 0, revision = 0;
    bool snapshot = false;
    std::map<std::string, AttributeValue> values;
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
