#include "fsnet/database.hpp"
#include <stdexcept>
#ifdef FSNET_WITH_POSTGRES
#include <libpq-fe.h>
#include <algorithm>
#include <bit>
#include <limits>
#include <mutex>
#include <unordered_set>

namespace fsnet {
namespace {
struct ClearResult { void operator()(PGresult* result) const { if (result) PQclear(result); } };
using Query = std::unique_ptr<PGresult, ClearResult>;
class Postgres final : public Database {
    PGconn* connection_ = nullptr;
    std::mutex mutex_;
    std::string ssl_mode_;
    static bool loopback(std::string_view host) { return host == "localhost" || host == "127.0.0.1" || host == "::1" || host.starts_with('/'); }
    [[noreturn]] void fail(PGresult* result = nullptr) {
        const char* state = result ? PQresultErrorField(result, PG_DIAG_SQLSTATE) : nullptr;
        throw std::runtime_error(std::string("PostgreSQL operation failed") + (state ? " (SQLSTATE " + std::string(state) + ')' : ""));
    }
    Query query(const char* sql, const std::vector<std::string_view>& values = {}, bool binary_result = false) {
        std::vector<const char*> inputs; std::vector<int> lengths, formats; std::vector<Oid> types;
        for (auto value : values) { inputs.push_back(value.data()); lengths.push_back(static_cast<int>(value.size())); formats.push_back(1); types.push_back(17); }
        Query result(PQexecParams(connection_, sql, static_cast<int>(values.size()), types.data(), inputs.data(), lengths.data(), formats.data(), binary_result ? 1 : 0));
        if (!result || (PQresultStatus(result.get()) != PGRES_COMMAND_OK && PQresultStatus(result.get()) != PGRES_TUPLES_OK)) fail(result.get());
        return result;
    }
    void ready() {
        if (PQstatus(connection_) != CONNECTION_OK) PQreset(connection_);
        if (PQstatus(connection_) != CONNECTION_OK) fail();
        const auto* address = PQhostaddr(connection_);
        if ((!loopback(PQhost(connection_)) || (address && *address && !loopback(address))) &&
            (ssl_mode_ != "verify-full" || !PQsslInUse(connection_))) throw std::runtime_error("remote PostgreSQL requires verified TLS");
    }
    std::optional<Record> read(std::string_view key, bool locked = false) {
        auto result = query(locked ? "SELECT value,version FROM fsnet_records WHERE key=$1 FOR UPDATE" : "SELECT value,version FROM fsnet_records WHERE key=$1", {key}, true);
        if (!PQntuples(result.get())) return std::nullopt;
        if (PQgetlength(result.get(), 0, 1) != 8) throw std::runtime_error("invalid PostgreSQL revision");
        std::uint64_t version = 0; auto* bytes = PQgetvalue(result.get(), 0, 1);
        for (int i = 0; i < 8; ++i) version = (version << 8) | static_cast<unsigned char>(bytes[i]);
        return Record{std::string(PQgetvalue(result.get(), 0, 0), PQgetlength(result.get(), 0, 0)), version};
    }

public:
    explicit Postgres(const std::string& options) {
        if (options.find('\0') != std::string::npos) throw std::invalid_argument("invalid PostgreSQL connection options");
        char* parse_error = nullptr; auto* parsed = PQconninfoParse(options.c_str(), &parse_error);
        if (!parsed) { if (parse_error) PQfreemem(parse_error); throw std::invalid_argument("invalid PostgreSQL connection options"); }
        using Options=std::unique_ptr<PQconninfoOption,decltype(&PQconninfoFree)>;
        Options explicit_options(parsed,PQconninfoFree),defaults(PQconndefaults(),PQconninfoFree);
        if(!defaults)throw std::runtime_error("PostgreSQL defaults unavailable");
        ssl_mode_ = "verify-full";
        std::string host,hostaddr,service;
        for(auto* source:{defaults.get(),explicit_options.get()})for(auto* entry=source;entry->keyword;++entry){
            if(!entry->val||!*entry->val)continue;
            std::string_view name(entry->keyword);
            if(name=="host")host=entry->val;
            if(name=="hostaddr")hostaddr=entry->val;
            if(name=="service")service=entry->val;
            if(source==explicit_options.get()&&name=="sslmode")ssl_mode_=entry->val;
        }
        // Environment defaults and service expansion are connection inputs too.
        // A service's destination cannot be proved local before expansion, so
        // it always requires verified TLS before authentication starts.
        bool remote = !service.empty();
        for (std::string_view value : {std::string_view(host),std::string_view(hostaddr)}) {
            if(!value.empty()) {
                std::size_t offset = 0, count = 0;
                while (offset < value.size()) {
                    auto end = value.find(',', offset); auto host = value.substr(offset, end == std::string_view::npos ? end : end - offset);
                    if (!loopback(host)) remote = true;
                    if (++count > 8) throw std::invalid_argument("too many PostgreSQL destinations");
                    if (end == std::string_view::npos) break;
                    offset = end + 1;
                }
            }
        }
        if (remote && ssl_mode_ != "verify-full") throw std::invalid_argument("remote PostgreSQL requires sslmode=verify-full");
        const char* keys[] = {"dbname", "sslmode", "host", "hostaddr", "connect_timeout", "target_session_attrs", "options", nullptr};
        const char* values[] = {options.c_str(), ssl_mode_.c_str(),host.c_str(),hostaddr.c_str(), "5", "read-write", "-c statement_timeout=5000 -c lock_timeout=5000 -c synchronous_commit=on", nullptr};
        connection_ = PQconnectdbParams(keys, values, 1);
        try {
            if (!connection_ || PQstatus(connection_) != CONNECTION_OK) fail();
            ready();
            query("SELECT pg_advisory_lock(527941985139)");
            query("CREATE TABLE IF NOT EXISTS fsnet_records (key bytea PRIMARY KEY, value bytea NOT NULL, version bigint NOT NULL CHECK(version>0))");
            query("CREATE SEQUENCE IF NOT EXISTS fsnet_record_revision MINVALUE 1 MAXVALUE 9223372036854775806");
            query("SELECT pg_advisory_unlock(527941985139)");
        } catch (...) { if (connection_) PQfinish(connection_); connection_ = nullptr; throw; }
    }
    ~Postgres() override { if (connection_) PQfinish(connection_); }
    std::optional<Record> get(std::string_view key) override {
        if (key.size() > 4096) throw std::invalid_argument("storage key too large");
        std::lock_guard lock(mutex_); ready();
        try { return read(key); } catch (...) {
            if (PQstatus(connection_) != CONNECTION_BAD) throw;
            ready(); return read(key); // a read can safely reconnect; writes are never replayed here
        }
    }
    bool compare_exchange(std::string_view key, std::uint64_t expected, std::string_view value) override {
        if (key.empty() || key.size() > 4096 || value.size() > 16 * 1024 * 1024 || expected >= static_cast<std::uint64_t>(INT64_MAX))
            throw std::invalid_argument("invalid storage replacement");
        std::lock_guard lock(mutex_); ready();
        auto sql = expected ? "UPDATE fsnet_records SET value=$2,version=nextval('fsnet_record_revision') WHERE key=$1 AND version=" + std::to_string(expected) + " RETURNING version" :
            std::string("INSERT INTO fsnet_records(key,value,version) VALUES($1,$2,nextval('fsnet_record_revision')) ON CONFLICT DO NOTHING RETURNING version");
        auto result = query(sql.c_str(), {key, value}); return PQntuples(result.get()) == 1;
    }
    bool erase(std::string_view key, std::uint64_t expected) override {
        if (key.empty() || key.size() > 4096 || expected >= static_cast<std::uint64_t>(INT64_MAX)) throw std::invalid_argument("invalid storage erase");
        if (!expected) return false;
        std::lock_guard lock(mutex_); ready();
        auto result = query(("DELETE FROM fsnet_records WHERE key=$1 AND version=" + std::to_string(expected) + " RETURNING version").c_str(), {key});
        return PQntuples(result.get()) == 1;
    }
};
}
std::shared_ptr<Database> postgres_database(const std::string& options) { return std::make_shared<Postgres>(options); }
}
#else
namespace fsnet {
std::shared_ptr<Database> postgres_database(const std::string&) { throw std::runtime_error("PostgreSQL adapter requires FSNET_WITH_POSTGRES=ON"); }
}
#endif
