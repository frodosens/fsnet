#pragma once
#include "fsnet/entity_schema.hpp"
#include <string_view>

namespace fsnet {
// Bounded, language-neutral FSA1 record used for both durable property snapshots
// and replicated snapshots/deltas. Entity identity belongs to the enclosing frame.
std::string encode_attributes(const AttributeUpdate&);
AttributeUpdate decode_attributes(std::string_view);
// Client-side projection. A fresh subscription requires a snapshot. Gaps return
// false without mutation so the SDK can request resynchronization.
class AttributeReplica {
public:
    bool apply(const AttributeUpdate&);
    bool apply(std::string_view bytes) { return apply(decode_attributes(bytes)); }
    const std::map<std::string, AttributeValue>& values() const { return values_; }
    std::uint64_t revision() const { return revision_; }
    bool initialized() const { return initialized_; }
    void reset() { values_.clear(); revision_ = 0; initialized_ = false; }
private:
    std::map<std::string, AttributeValue> values_;
    std::uint64_t revision_ = 0;
    bool initialized_ = false;
};
}
