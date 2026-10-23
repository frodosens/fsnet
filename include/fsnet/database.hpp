#pragma once
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace fsnet {
struct Record {
    std::string value;
    std::uint64_t version;
};
struct Mutation {
    std::string key;
    std::uint64_t expected;
    std::optional<std::string> value;
    bool check_only = false; // Test a version without changing that record.
};
// A storage contract, not a SQL abstraction. Implementations must make CAS atomic.
// expected=0 creates a missing key; erase also requires the observed version.
class Database {
public:
    virtual ~Database() = default;
    virtual std::optional<Record> get(std::string_view key) = 0;
    // Atomic multi-key mutation. Revisions must never be reused after deletion.
    virtual bool transact(const std::vector<Mutation>& mutations) = 0;
    bool compare_exchange(std::string_view key, std::uint64_t expected, std::string_view value) {
        return transact({{std::string(key), expected, std::string(value)}});
    }
    bool erase(std::string_view key, std::uint64_t expected) {
        return transact({{std::string(key), expected, std::nullopt}});
    }
};
std::shared_ptr<Database> memory_database();
std::shared_ptr<Database> sqlite_database(const std::string& filename);
}
