#pragma once
#include <cstdint>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace fsnet {
struct Record { std::string value; std::uint64_t version; };
struct Mutation {
    std::string key;
    std::uint64_t expected;
    std::optional<std::string> value;
    bool check_only = false;
};
// Minimum persistence contract: linearizable, durable single-record CAS. No SQL,
// multi-record transaction or shared database server is required by the runtime.
// expected=0 creates an absent key. Revisions cannot repeat after delete/recreate.
class Storage {
public:
    virtual ~Storage() = default;
    virtual std::optional<Record> get(std::string_view key) = 0;
    virtual bool compare_exchange(std::string_view key, std::uint64_t expected, std::string_view value) = 0;
    virtual bool erase(std::string_view key, std::uint64_t expected) = 0;
    virtual bool supports_transactions() const { return false; }
    // Optional application extension. Framework ownership/state paths never call it.
    virtual bool transact(const std::vector<Mutation>&) { throw std::logic_error("storage does not support multi-record transactions"); }
};
std::shared_ptr<Storage> file_storage(const std::string& directory);
std::shared_ptr<Storage> open_storage(const std::string& uri);
}
