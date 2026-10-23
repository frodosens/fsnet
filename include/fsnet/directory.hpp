#pragma once
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

namespace fsnet {
// Local view populated by deployment/control-plane heartbeats. This is not a
// consensus store: it never grants exclusive ownership of persistent entities.
class ServiceDirectory {
    using Clock = std::chrono::steady_clock;
    struct Entry { std::uint64_t peer; Clock::time_point expires; };
    std::unordered_map<std::string, std::vector<Entry>> services_;
    static std::uint64_t score(const std::string& key, std::uint64_t peer) {
        std::uint64_t hash = 14695981039346656037ULL;
        for (unsigned char byte : key) hash = (hash ^ byte) * 1099511628211ULL;
        for (unsigned i = 0; i < 8; ++i) hash = (hash ^ ((peer >> (i * 8)) & 255)) * 1099511628211ULL;
        return hash;
    }
public:
    void renew(const std::string& service, std::uint64_t peer, std::chrono::milliseconds ttl,
               Clock::time_point now = Clock::now()) {
        if (service.empty() || !peer || ttl.count() <= 0) throw std::invalid_argument("invalid service lease");
        auto& entries = services_[service];
        auto it = std::find_if(entries.begin(), entries.end(), [peer](auto& entry) { return entry.peer == peer; });
        if (it == entries.end()) entries.push_back({peer, now + ttl}); else it->expires = now + ttl;
    }
    void remove(std::uint64_t peer) {
        for (auto& [name, entries] : services_) std::erase_if(entries, [peer](auto& entry) { return entry.peer == peer; });
    }
    std::optional<std::uint64_t> locate(const std::string& service, const std::string& key,
                                      Clock::time_point now = Clock::now()) const {
        auto it = services_.find(service);
        if (it == services_.end()) return std::nullopt;
        std::optional<std::uint64_t> selected;
        std::uint64_t best = 0;
        for (auto& entry : it->second) if (entry.expires > now) {
            auto value = score(key, entry.peer);
            if (!selected || value > best || (value == best && entry.peer < *selected)) { selected = entry.peer; best = value; }
        }
        return selected;
    }
};
}
