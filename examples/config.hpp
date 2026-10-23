#pragma once
#include "fsnet/node.hpp"
#include <cstdlib>

inline fsnet::TlsConfig tls_from_environment() {
    auto value = [](const char* name) -> std::string { auto* text = std::getenv(name); return text ? text : ""; };
    return {value("FSNET_TLS_CA"), value("FSNET_TLS_CERTIFICATE"), value("FSNET_TLS_KEY"), value("FSNET_TLS_PEER_NAME")};
}
inline std::string bind_from_environment() {
    auto* address = std::getenv("FSNET_BIND_ADDRESS"); return address ? address : "127.0.0.1";
}
