#include "fsnet/attribute_wire.hpp"
#include <bit>
#include <cmath>
#include <limits>

namespace fsnet {
namespace {
constexpr std::size_t limit = 1024 * 1024;
void put(std::string& out, std::uint64_t n, unsigned count) {
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
}
std::string encode_attributes(const AttributeUpdate& update) {
    if (update.values.size() > 256 || update.base > update.revision || (update.snapshot && update.base))
        throw std::invalid_argument("invalid attribute revision or count");
    std::string out = "FSA1"; put(out, update.snapshot, 1); put(out, update.base, 8); put(out, update.revision, 8);
    put(out, update.values.size(), 2);
    for (const auto& [name, value] : update.values) {
        check_name(name); put(out, name.size(), 1); out += name; put(out, value.index(), 1);
        if (auto n = std::get_if<std::int64_t>(&value)) put(out, std::bit_cast<std::uint64_t>(*n), 8);
        else if (auto d = std::get_if<double>(&value)) {
            if (!std::isfinite(*d)) throw std::invalid_argument("non-finite attribute");
            put(out, std::bit_cast<std::uint64_t>(*d), 8);
        } else if (auto b = std::get_if<bool>(&value)) put(out, *b, 1);
        else {
            const auto& s = std::get<std::string>(value);
            if (s.size() > 65536) throw std::length_error("property string too large");
            put(out, s.size(), 4); out += s;
        }
        if (out.size() > limit) throw std::length_error("attribute image too large");
    }
    return out;
}
AttributeUpdate decode_attributes(std::string_view in) {
    if (in.size() > limit || !in.starts_with("FSA1")) throw std::invalid_argument("invalid attribute record");
    in.remove_prefix(4); auto flag = get(in, 1);
    if (flag > 1) throw std::invalid_argument("invalid snapshot flag");
    AttributeUpdate result; result.snapshot = flag; result.base = get(in, 8); result.revision = get(in, 8);
    auto count = get(in, 2);
    if (count > 256 || result.base > result.revision || (result.snapshot && result.base)) throw std::invalid_argument("invalid attribute header");
    for (std::uint64_t i = 0; i < count; ++i) {
        auto size = get(in, 1);
        if (size > in.size()) throw std::invalid_argument("truncated attribute name");
        std::string name(in.substr(0, size)); in.remove_prefix(size); check_name(name);
        auto tag = get(in, 1); AttributeValue value;
        if (tag == 0) value = std::bit_cast<std::int64_t>(get(in, 8));
        else if (tag == 1) {
            auto d = std::bit_cast<double>(get(in, 8));
            if (!std::isfinite(d)) throw std::invalid_argument("non-finite attribute");
            value = d;
        } else if (tag == 2) {
            auto b = get(in, 1); if (b > 1) throw std::invalid_argument("invalid boolean"); value = bool(b);
        } else if (tag == 3) {
            auto length = get(in, 4);
            if (length > 65536 || length > in.size()) throw std::invalid_argument("invalid property string");
            value = std::string(in.substr(0, length)); in.remove_prefix(length);
        } else throw std::invalid_argument("invalid property type");
        if (!result.values.emplace(std::move(name), std::move(value)).second) throw std::invalid_argument("duplicate property");
    }
    if (!in.empty()) throw std::invalid_argument("trailing attribute bytes");
    return result;
}
bool AttributeReplica::apply(const AttributeUpdate& update) {
    // Validate programmatic updates too; never apply partially on validation or allocation failure.
    (void)encode_attributes(update);
    if (!update.snapshot && (!initialized_ || update.base != revision_)) return false;
    if (initialized_ && update.revision < revision_) return false;
    auto next = update.snapshot ? std::map<std::string, AttributeValue>{} : values_;
    for (const auto& [name, value] : update.values) next.insert_or_assign(name, value);
    (void)encode_attributes({0, update.revision, true, next});
    values_.swap(next); revision_ = update.revision; initialized_ = true; return true;
}
}
