#include "fsnet/database.hpp"
#include <SQLiteCpp/SQLiteCpp.h>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>

namespace fsnet {
namespace {
constexpr auto max_version = static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
void validate(const std::vector<Mutation>& mutations) {
    if (mutations.size() > 1024) throw std::length_error("transaction too large");
    std::unordered_set<std::string> keys;
    for (auto& mutation : mutations) {
        if (mutation.expected >= max_version || mutation.key.size() > 4096 || mutation.key.empty() ||
            (mutation.value && mutation.value->size() > 16 * 1024 * 1024))
            throw std::invalid_argument("invalid storage mutation");
        if (!keys.insert(mutation.key).second) throw std::invalid_argument("duplicate transaction key");
    }
}
class Memory final : public Database {
    std::mutex mutex_;
    std::unordered_map<std::string, Record> records_;
    std::uint64_t revision_ = 0;
public:
    std::optional<Record> get(std::string_view key) override {
        std::lock_guard lock(mutex_);
        auto it = records_.find(std::string(key));
        return it == records_.end() ? std::nullopt : std::optional(it->second);
    }
    bool supports_transactions() const override { return true; }
    bool compare_exchange(std::string_view key, std::uint64_t expected, std::string_view value) override {
        return transact({{std::string(key), expected, std::string(value)}});
    }
    bool erase(std::string_view key, std::uint64_t expected) override {
        return transact({{std::string(key), expected, std::nullopt}});
    }
    bool transact(const std::vector<Mutation>& mutations) override {
        validate(mutations);
        std::lock_guard lock(mutex_);
        for (auto& mutation : mutations) {
            auto it = records_.find(mutation.key);
            auto version = it == records_.end() ? 0 : it->second.version;
            if (version != mutation.expected || (!mutation.check_only && !mutation.value && !version)) return false;
        }
        if (mutations.size() >= max_version - revision_) throw std::overflow_error("record version exhausted");
        // Development backend: copy-on-commit makes allocation failures atomic.
        auto next = records_;
        auto revision = revision_;
        for (auto& mutation : mutations) {
            if (mutation.check_only) continue;
            if (mutation.value) next[mutation.key] = {*mutation.value, ++revision};
            else next.erase(mutation.key);
        }
        records_.swap(next); revision_ = revision;
        return true;
    }
};
class Sqlite final : public Database {
    std::mutex mutex_;
    SQLite::Database db_;
    std::optional<Record> read(std::string_view key) {
        SQLite::Statement query(db_, "SELECT value,version FROM fsnet_records WHERE key=?");
        query.bind(1, std::string(key));
        if (!query.executeStep()) return std::nullopt;
        auto data = query.getColumn(0);
        return Record{data.getBytes() ? std::string(static_cast<const char*>(data.getBlob()), data.getBytes()) : std::string{},
                      static_cast<std::uint64_t>(query.getColumn(1).getInt64())};
    }
public:
    explicit Sqlite(const std::string& file)
      : db_(file, SQLite::OPEN_READWRITE | SQLite::OPEN_CREATE | SQLite::OPEN_FULLMUTEX) {
        db_.setBusyTimeout(5000);
        db_.exec("PRAGMA journal_mode=WAL; PRAGMA synchronous=FULL;"
                 "CREATE TABLE IF NOT EXISTS fsnet_records (key TEXT PRIMARY KEY, value BLOB NOT NULL, version INTEGER NOT NULL CHECK(version>0));"
                 "CREATE TABLE IF NOT EXISTS fsnet_revision (id INTEGER PRIMARY KEY CHECK(id=1), version INTEGER NOT NULL);"
                 "INSERT OR IGNORE INTO fsnet_revision VALUES(1,0);"
                 "UPDATE fsnet_revision SET version=MAX(version,COALESCE((SELECT MAX(version) FROM fsnet_records),0));");
    }
    std::optional<Record> get(std::string_view key) override {
        std::lock_guard lock(mutex_); return read(key);
    }
    bool supports_transactions() const override { return true; }
    bool compare_exchange(std::string_view key, std::uint64_t expected, std::string_view value) override {
        return transact({{std::string(key), expected, std::string(value)}});
    }
    bool erase(std::string_view key, std::uint64_t expected) override {
        return transact({{std::string(key), expected, std::nullopt}});
    }
    bool transact(const std::vector<Mutation>& mutations) override {
        validate(mutations);
        std::lock_guard lock(mutex_);
        SQLite::Transaction transaction(db_, SQLite::TransactionBehavior::IMMEDIATE);
        for (auto& mutation : mutations) {
            auto record = read(mutation.key);
            auto version = record ? record->version : 0;
            if (version != mutation.expected || (!mutation.check_only && !mutation.value && !version)) return false;
        }
        auto revision = static_cast<std::uint64_t>(db_.execAndGet("SELECT version FROM fsnet_revision WHERE id=1").getInt64());
        if (mutations.size() >= max_version - revision) throw std::overflow_error("record version exhausted");
        for (auto& mutation : mutations) {
            if (mutation.check_only) continue;
            if (mutation.value) {
                SQLite::Statement write(db_, "INSERT INTO fsnet_records(key,value,version) VALUES(?,?,?) ON CONFLICT(key) DO UPDATE SET value=excluded.value,version=excluded.version");
                write.bind(1, mutation.key);
                write.bind(2, mutation.value->data(), static_cast<int>(mutation.value->size()));
                write.bind(3, static_cast<std::int64_t>(++revision)); write.exec();
            } else {
                SQLite::Statement remove(db_, "DELETE FROM fsnet_records WHERE key=?");
                remove.bind(1, mutation.key); remove.exec();
            }
        }
        SQLite::Statement sequence(db_, "UPDATE fsnet_revision SET version=? WHERE id=1");
        sequence.bind(1, static_cast<std::int64_t>(revision)); sequence.exec();
        transaction.commit(); return true;
    }
};
}
std::shared_ptr<Database> memory_database() { return std::make_shared<Memory>(); }
std::shared_ptr<Database> sqlite_database(const std::string& file) { return std::make_shared<Sqlite>(file); }
}
