#include "fsnet/ice.hpp"
#include <juice/juice.h>
#include <atomic>
#include <mutex>
#include <stdexcept>
#include <utility>

namespace fsnet {
namespace {
void check(int code) { if (code < 0) throw std::runtime_error("ICE operation failed: " + std::to_string(code)); }
}
struct IcePeer::Impl {
    juice_agent_t* agent = nullptr;
    mutable std::mutex mutex;
    std::vector<std::string> candidates, messages;
    bool done = false;
};
IcePeer::IcePeer(const IceConfig& options) : impl_(std::make_unique<Impl>()) {
    juice_config_t config{};
    juice_turn_server_t turn{};
    config.bind_address = options.bind_address.empty() ? nullptr : options.bind_address.c_str();
    if (!options.stun_host.empty()) {
        config.stun_server_host = options.stun_host.c_str(); config.stun_server_port = options.stun_port;
    }
    if (!options.turn_host.empty()) {
        if (options.turn_user.empty() || options.turn_password.empty()) throw std::invalid_argument("TURN credentials required");
        turn.host = options.turn_host.c_str(); turn.port = options.turn_port;
        turn.username = options.turn_user.c_str(); turn.password = options.turn_password.c_str();
        config.turn_servers = &turn; config.turn_servers_count = 1;
    }
    config.user_ptr = impl_.get();
    config.cb_candidate = [](juice_agent_t*, const char* candidate, void* data) {
        auto& self = *static_cast<Impl*>(data);
        std::lock_guard lock(self.mutex);
        if (self.candidates.size() < 64) self.candidates.emplace_back(candidate);
    };
    config.cb_recv = [](juice_agent_t*, const char* bytes, std::size_t size, void* data) {
        auto& self = *static_cast<Impl*>(data);
        std::lock_guard lock(self.mutex);
        if (size <= 1200 && self.messages.size() < 256) self.messages.emplace_back(bytes, size);
    };
    config.cb_gathering_done = [](juice_agent_t*, void* data) { auto& self = *static_cast<Impl*>(data); std::lock_guard lock(self.mutex); self.done = true; };
    impl_->agent = juice_create(&config);
    if (!impl_->agent) throw std::runtime_error("ICE initialization failed");
}
IcePeer::~IcePeer() { juice_destroy(impl_->agent); }
std::string IcePeer::description() const {
    char buffer[JUICE_MAX_SDP_STRING_LEN];
    check(juice_get_local_description(impl_->agent, buffer, sizeof buffer)); return buffer;
}
void IcePeer::remote_description(const std::string& sdp) {
    if (sdp.size() >= JUICE_MAX_SDP_STRING_LEN || sdp.find('\0') != std::string::npos) throw std::invalid_argument("invalid ICE description");
    check(juice_set_remote_description(impl_->agent, sdp.c_str()));
}
void IcePeer::add_candidate(const std::string& candidate) {
    if (candidate.size() >= JUICE_MAX_CANDIDATE_SDP_STRING_LEN || candidate.find('\0') != std::string::npos) throw std::invalid_argument("invalid ICE candidate");
    check(juice_add_remote_candidate(impl_->agent, candidate.c_str()));
}
void IcePeer::remote_gathering_done() { check(juice_set_remote_gathering_done(impl_->agent)); }
void IcePeer::gather() { check(juice_gather_candidates(impl_->agent)); }
IceGathering IcePeer::gathering() {
    std::lock_guard lock(impl_->mutex); return {std::exchange(impl_->candidates, {}), impl_->done};
}
std::vector<std::string> IcePeer::candidates() {
    std::lock_guard lock(impl_->mutex); return std::exchange(impl_->candidates, {});
}
std::vector<std::string> IcePeer::receive() {
    std::lock_guard lock(impl_->mutex); return std::exchange(impl_->messages, {});
}
bool IcePeer::gathering_done() const { std::lock_guard lock(impl_->mutex); return impl_->done && impl_->candidates.empty(); }
bool IcePeer::connected() const {
    auto state = juice_get_state(impl_->agent); return state == JUICE_STATE_CONNECTED || state == JUICE_STATE_COMPLETED;
}
bool IcePeer::send(const std::string& data) {
    if (data.size() > 1200) return false;
    return juice_send(impl_->agent, data.data(), data.size()) == 0;
}
std::string IcePeer::selected_candidates() const {
    char local[JUICE_MAX_CANDIDATE_SDP_STRING_LEN], remote[JUICE_MAX_CANDIDATE_SDP_STRING_LEN];
    check(juice_get_selected_candidates(impl_->agent, local, sizeof local, remote, sizeof remote));
    return std::string(local) + "\n" + remote;
}
struct TurnRelay::Impl { juice_server_t* server = nullptr; };
TurnRelay::TurnRelay(const RelayConfig& options) : impl_(std::make_unique<Impl>()) {
    if (options.max_allocations <= 0 || options.max_peers <= 0 || options.relay_begin > options.relay_end)
        throw std::invalid_argument("invalid relay limits");
    juice_server_config_t config{};
    config.bind_address = options.bind_address.c_str();
    config.external_address = options.external_address.empty() ? nullptr : options.external_address.c_str();
    config.port = options.port; config.relay_port_range_begin = options.relay_begin; config.relay_port_range_end = options.relay_end;
    config.max_allocations = options.max_allocations; config.max_peers = options.max_peers;
    config.realm = "fsnet";
    impl_->server = juice_server_create(&config);
    if (!impl_->server) throw std::runtime_error("TURN relay initialization failed");
}
TurnRelay::~TurnRelay() { juice_server_destroy(impl_->server); }
std::uint16_t TurnRelay::port() const { return juice_server_get_port(impl_->server); }
void TurnRelay::credentials(const std::string& user, const std::string& password, unsigned long lifetime_ms, int quota) {
    if (user.empty() || password.size() < 16 || !lifetime_ms || quota <= 0) throw std::invalid_argument("invalid TURN credential or quota");
    juice_server_credentials_t credentials{};
    credentials.username = user.c_str(); credentials.password = password.c_str(); credentials.allocations_quota = quota;
    check(juice_server_add_credentials(impl_->server, &credentials, lifetime_ms));
}
}
