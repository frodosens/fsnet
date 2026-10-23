#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace fsnet {
struct IceConfig {
    std::string bind_address;
    std::string stun_host;
    std::uint16_t stun_port = 3478;
    std::string turn_host, turn_user, turn_password;
    std::uint16_t turn_port = 3478;
};
// ICE signaling (descriptions/candidates) travels through the authenticated game
// service. UDP payloads are unordered/unreliable and must be encrypted by the app.
struct IceGathering { std::vector<std::string> candidates; bool done = false; };
class IcePeer {
public:
    explicit IcePeer(const IceConfig& config = {});
    ~IcePeer();
    IcePeer(const IcePeer&) = delete;
    IcePeer& operator=(const IcePeer&) = delete;
    std::string description() const;
    void remote_description(const std::string& description);
    void add_candidate(const std::string& candidate);
    void remote_gathering_done();
    void gather();
    IceGathering gathering(); // atomically drain candidates with their end-of-gathering flag
    std::vector<std::string> candidates();
    std::vector<std::string> receive();
    bool gathering_done() const;
    bool connected() const;
    bool send(const std::string& data);
    std::string selected_candidates() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
struct RelayConfig {
    std::string bind_address = "127.0.0.1";
    std::string external_address;
    std::uint16_t port = 0;
    std::uint16_t relay_begin = 0, relay_end = 0;
    int max_allocations = 128, max_peers = 16;
};
class TurnRelay {
public:
    explicit TurnRelay(const RelayConfig& config = {});
    ~TurnRelay();
    TurnRelay(const TurnRelay&) = delete;
    TurnRelay& operator=(const TurnRelay&) = delete;
    std::uint16_t port() const;
    void credentials(const std::string& user, const std::string& password,
                     unsigned long lifetime_ms, int quota = 2);
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
