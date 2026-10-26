#include "fsnet/database.hpp"
#include <stdexcept>
#ifdef FSNET_WITH_POSTGRES
#include <libpq-fe.h>
#include <arpa/inet.h>
#include <algorithm>
#include <bit>
#include <cerrno>
#include <chrono>
#include <climits>
#include <limits>
#include <mutex>
#include <poll.h>
#include <unordered_set>

namespace fsnet {
namespace {
struct ClearResult { void operator()(PGresult* result) const { if (result) PQclear(result); } };
using Query = std::unique_ptr<PGresult, ClearResult>;
class Postgres final : public Storage {
    using Clock = std::chrono::steady_clock;
    using Deadline = Clock::time_point;
    static constexpr auto operation_timeout = std::chrono::seconds(5);
    PGconn* connection_ = nullptr;
    std::timed_mutex mutex_;
    std::string options_, ssl_mode_, host_, hostaddr_;
    static bool loopback(std::string_view host) { return host == "localhost" || host == "127.0.0.1" || host == "::1" || host.starts_with('/'); }
    static bool numeric(const std::string& address) {
        in_addr ipv4{}; in6_addr ipv6{};
        return ::inet_pton(AF_INET, address.c_str(), &ipv4) == 1 || ::inet_pton(AF_INET6, address.c_str(), &ipv6) == 1;
    }
    static std::vector<std::string> destinations(std::string_view value) {
        std::vector<std::string> result;
        for (;;) {
            const auto end = value.find(',');
            result.emplace_back(value.substr(0, end));
            if (result.size() > 8) throw std::invalid_argument("too many PostgreSQL destinations");
            if (end == std::string_view::npos) return result;
            value.remove_prefix(end + 1);
        }
    }
    static std::string numeric_destinations(const std::string& host, const std::string& hostaddr, bool service) {
        // libpq's async connect API still calls the synchronous OS resolver for
        // a hostname. Require numeric addresses before entering libpq, while
        // retaining host for verify-full certificate name validation.
        if (service && (host.empty() || hostaddr.empty()))
            throw std::invalid_argument("PostgreSQL service requires explicit host and numeric hostaddr");
        const auto hosts = destinations(host);
        auto addresses = hostaddr.empty() ? std::vector<std::string>(hosts.size()) : destinations(hostaddr);
        if (addresses.size() != hosts.size()) throw std::invalid_argument("PostgreSQL host and hostaddr counts differ");
        std::string result;
        for (std::size_t i = 0; i < hosts.size(); ++i) {
            auto& address = addresses[i];
            if (address.empty()) {
                if (hosts[i] == "localhost") address = "127.0.0.1";
                else if (numeric(hosts[i])) address = hosts[i];
                else if (!hosts[i].empty() && !hosts[i].starts_with('/'))
                    throw std::invalid_argument("PostgreSQL DNS host requires numeric hostaddr");
            } else if (!numeric(address)) throw std::invalid_argument("PostgreSQL hostaddr must be numeric");
            if (i) result += ',';
            result += address;
        }
        return result;
    }
    [[noreturn]] void fail(PGresult* result = nullptr) {
        const char* state = result ? PQresultErrorField(result, PG_DIAG_SQLSTATE) : nullptr;
        throw std::runtime_error(std::string("PostgreSQL operation failed") + (state ? " (SQLSTATE " + std::string(state) + ')' : ""));
    }
    void disconnect() noexcept {
        if (connection_) PQfinish(connection_);
        connection_ = nullptr;
    }
    static void check_deadline(Deadline deadline) {
        if (Clock::now() >= deadline) throw std::runtime_error("PostgreSQL operation deadline exceeded");
    }
    std::unique_lock<std::timed_mutex> lock(Deadline deadline) {
        std::unique_lock<std::timed_mutex> result(mutex_, std::defer_lock);
        if (!result.try_lock_until(deadline)) throw std::runtime_error("PostgreSQL operation deadline exceeded");
        return result;
    }
    short wait(short events, Deadline deadline) {
        for (;;) {
            check_deadline(deadline);
            const auto remaining = std::chrono::ceil<std::chrono::milliseconds>(deadline - Clock::now());
            pollfd socket{PQsocket(connection_), events, 0};
            if (socket.fd < 0) fail();
            auto timeout = static_cast<int>(std::clamp<std::int64_t>(remaining.count(), 1, INT_MAX));
            const auto result = ::poll(&socket, 1, timeout);
            if (result < 0 && errno == EINTR) continue;
            if (result < 0 || (socket.revents & POLLNVAL)) fail();
            if (!result) continue; // recheck the same absolute deadline
            return socket.revents;
        }
    }
    void connect(Deadline deadline) {
        check_deadline(deadline);
        const char* keys[] = {"dbname", "sslmode", "host", "hostaddr", "connect_timeout", "target_session_attrs", "options", nullptr};
        const char* values[] = {options_.c_str(), ssl_mode_.c_str(), host_.c_str(), hostaddr_.c_str(), "5", "read-write", "-c statement_timeout=5000 -c lock_timeout=5000 -c synchronous_commit=on", nullptr};
        connection_ = PQconnectStartParams(keys, values, 1);
        try {
            if (!connection_) fail();
            for (;;) {
                check_deadline(deadline);
                const auto status = PQconnectPoll(connection_);
                if (status == PGRES_POLLING_OK) break;
                if (status == PGRES_POLLING_FAILED) fail();
                if (status == PGRES_POLLING_READING) wait(POLLIN, deadline);
                else if (status == PGRES_POLLING_WRITING) wait(POLLOUT, deadline);
            }
            check_deadline(deadline);
            if (PQsetnonblocking(connection_, 1)) fail();
        } catch (...) { disconnect(); throw; }
    }
    Query query(Deadline deadline, const char* sql, const std::vector<std::string_view>& values = {}, bool binary_result = false) {
        check_deadline(deadline);
        std::vector<const char*> inputs; std::vector<int> lengths, formats; std::vector<Oid> types;
        for (auto value : values) { inputs.push_back(value.data()); lengths.push_back(static_cast<int>(value.size())); formats.push_back(1); types.push_back(17); }
        bool drained = false;
        try {
            if (!PQsendQueryParams(connection_, sql, static_cast<int>(values.size()), types.data(), inputs.data(), lengths.data(), formats.data(), binary_result ? 1 : 0)) fail();
            for (;;) {
                check_deadline(deadline);
                const auto pending = PQflush(connection_);
                if (pending < 0) fail();
                if (!pending) break;
                const auto events = wait(POLLIN | POLLOUT, deadline);
                if ((events & (POLLIN | POLLERR | POLLHUP)) && !PQconsumeInput(connection_)) fail();
            }
            Query result;
            for (;;) {
                check_deadline(deadline);
                while (PQisBusy(connection_)) {
                    wait(POLLIN, deadline);
                    if (!PQconsumeInput(connection_)) fail();
                }
                Query next(PQgetResult(connection_));
                if (!next) break;
                result = std::move(next);
            }
            drained = true;
            if (!result || (PQresultStatus(result.get()) != PGRES_COMMAND_OK && PQresultStatus(result.get()) != PGRES_TUPLES_OK)) fail(result.get());
            return result;
        } catch (...) {
            // A deadline/transport failure leaves command completion unknown.
            // Drop that connection; never replay a write on a fresh one.
            if (!drained || PQstatus(connection_) == CONNECTION_BAD) disconnect();
            throw;
        }
    }
    void ready(Deadline deadline) {
        check_deadline(deadline);
        if (!connection_ || PQstatus(connection_) != CONNECTION_OK) {
            disconnect();
            connect(deadline);
        }
        const auto* address = PQhostaddr(connection_);
        if ((!loopback(PQhost(connection_)) || (address && *address && !loopback(address))) &&
            (ssl_mode_ != "verify-full" || !PQsslInUse(connection_))) throw std::runtime_error("remote PostgreSQL requires verified TLS");
    }
    std::optional<Record> read(std::string_view key, Deadline deadline) {
        auto result = query(deadline, "SELECT value,version FROM fsnet_records WHERE key=$1", {key}, true);
        if (!PQntuples(result.get())) return std::nullopt;
        if (PQgetlength(result.get(), 0, 1) != 8) throw std::runtime_error("invalid PostgreSQL revision");
        std::uint64_t version = 0; auto* bytes = PQgetvalue(result.get(), 0, 1);
        for (int i = 0; i < 8; ++i) version = (version << 8) | static_cast<unsigned char>(bytes[i]);
        return Record{std::string(PQgetvalue(result.get(), 0, 0), PQgetlength(result.get(), 0, 0)), version};
    }

public:
    explicit Postgres(const std::string& options) : options_(options) {
        const auto deadline = Clock::now() + operation_timeout;
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
        hostaddr_ = numeric_destinations(host, hostaddr, !service.empty());
        host_ = std::move(host);
        try {
            ready(deadline);
            query(deadline, "SELECT pg_advisory_lock(527941985139)");
            query(deadline, "CREATE TABLE IF NOT EXISTS fsnet_records (key bytea PRIMARY KEY, value bytea NOT NULL, version bigint NOT NULL CHECK(version>0))");
            query(deadline, "CREATE SEQUENCE IF NOT EXISTS fsnet_record_revision MINVALUE 1 MAXVALUE 9223372036854775806");
            query(deadline, "SELECT pg_advisory_unlock(527941985139)");
        } catch (...) { disconnect(); throw; }
    }
    ~Postgres() override { if (connection_) PQfinish(connection_); }
    bool persistent() const noexcept override { return true; }
    std::optional<Record> get(std::string_view key) override {
        if (key.size() > 4096) throw std::invalid_argument("storage key too large");
        const auto deadline = Clock::now() + operation_timeout;
        auto guard = lock(deadline);
        ready(deadline);
        try { return read(key, deadline); } catch (...) {
            if (connection_ && PQstatus(connection_) != CONNECTION_BAD) throw;
            check_deadline(deadline);
            ready(deadline); return read(key, deadline); // only reads may retry, within the original deadline
        }
    }
    bool compare_exchange(std::string_view key, std::uint64_t expected, std::string_view value) override {
        if (key.empty() || key.size() > 4096 || value.size() > 16 * 1024 * 1024 || expected >= static_cast<std::uint64_t>(INT64_MAX))
            throw std::invalid_argument("invalid storage replacement");
        const auto deadline = Clock::now() + operation_timeout;
        auto guard = lock(deadline);
        ready(deadline);
        auto sql = expected ? "UPDATE fsnet_records SET value=$2,version=nextval('fsnet_record_revision') WHERE key=$1 AND version=" + std::to_string(expected) + " RETURNING version" :
            std::string("INSERT INTO fsnet_records(key,value,version) VALUES($1,$2,nextval('fsnet_record_revision')) ON CONFLICT DO NOTHING RETURNING version");
        auto result = query(deadline, sql.c_str(), {key, value}); return PQntuples(result.get()) == 1;
    }
    bool erase(std::string_view key, std::uint64_t expected) override {
        if (key.empty() || key.size() > 4096 || expected >= static_cast<std::uint64_t>(INT64_MAX)) throw std::invalid_argument("invalid storage erase");
        if (!expected) return false;
        const auto deadline = Clock::now() + operation_timeout;
        auto guard = lock(deadline);
        ready(deadline);
        auto result = query(deadline, ("DELETE FROM fsnet_records WHERE key=$1 AND version=" + std::to_string(expected) + " RETURNING version").c_str(), {key});
        return PQntuples(result.get()) == 1;
    }
};
}
std::shared_ptr<Storage> postgres_database(const std::string& options) { return std::make_shared<Postgres>(options); }
}
#else
namespace fsnet {
std::shared_ptr<Storage> postgres_database(const std::string&) { throw std::runtime_error("PostgreSQL adapter requires FSNET_WITH_POSTGRES=ON"); }
}
#endif
