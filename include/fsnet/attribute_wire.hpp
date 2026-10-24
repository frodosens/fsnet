#pragma once
#include "fsnet/entity_schema.hpp"
#include <string_view>
#include <optional>
#include <utility>

namespace fsnet {
struct AttributeWriteOptions {
    std::optional<std::uint64_t> expected_revision;
    std::string request_id;
};
// Bounded records: FSA1 snapshots/replacements, FSA2 container edits, FSA3
// typed field paths. Entity identity belongs to the enclosing frame.
std::string encode_attributes(const AttributeUpdate&);
// Exact encoded size with the same validation as encoding, without allocating
// a serialized record or copying the image into an AttributeUpdate.
std::size_t attribute_wire_size(const AttributeUpdate&);
std::size_t attribute_wire_size(const AttributeStruct&);
AttributeUpdate decode_attributes(std::string_view);
// Adaptive recursive field/container delta; falls back to FSA1 properties when
// that is smaller. Snapshots/durable images remain FSA1.
AttributeUpdate attribute_delta(const AttributeUpdate& previous, const AttributeUpdate& next);
// Client write proposals reuse attribute_delta, but retain explicitly submitted
// no-op roots for authorization/validation. Missing/stale baselines use FSA1.
AttributeUpdate attribute_write_delta(const AttributeStruct& confirmed, std::uint64_t confirmed_revision,
                                     const AttributeStruct& values, std::uint64_t expected_revision);
// Shared server-side staging: authorize roots, materialize using the replica
// patch applicator, then validate final values against the authoritative schema.
EntityAttributes::Mutation stage_attribute_proposal(const EntityAttributes&, const AttributeUpdate&, const CallIdentity&);
// Client-side projection. A fresh subscription requires a snapshot. Gaps return
// false without mutation so the SDK can request resynchronization.
class AttributeReplica {
public:
    AttributeReplica() = default;
    AttributeReplica(const AttributeReplica&) = default;
    AttributeReplica& operator=(const AttributeReplica&) = default;
    AttributeReplica(AttributeReplica&& other) noexcept
      : image_(std::move(other.image_)), revision_(std::exchange(other.revision_,0)), initialized_(std::exchange(other.initialized_,false)) {}
    AttributeReplica& operator=(AttributeReplica&& other) noexcept {
        if(this!=&other){image_=std::move(other.image_);revision_=std::exchange(other.revision_,0);initialized_=std::exchange(other.initialized_,false);}return *this;
    }
    bool apply(const AttributeUpdate&);
    bool apply(std::string_view bytes) { return apply(decode_attributes(bytes)); }
    // Borrowed until the next successful apply/reset. Use snapshot() to retain a version.
    const std::map<std::string, AttributeValue>& values() const { static const AttributeStruct empty; return image_?image_->values:empty; }
    std::shared_ptr<const AttributeUpdate> snapshot() const { return initialized_ ? image_ : nullptr; }
    std::uint64_t revision() const { return revision_; }
    bool initialized() const { return initialized_; }
    void reset() { image_.reset(); revision_ = 0; initialized_ = false; }
private:
    std::shared_ptr<AttributeUpdate> image_;
    std::uint64_t revision_ = 0;
    bool initialized_ = false;
};
}
