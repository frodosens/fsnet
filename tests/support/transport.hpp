#pragma once
#include <asio.hpp>
#include <openssl/hmac.h>
#include <array>
#include <chrono>
#include <stdexcept>
#include <string>
#include <thread>

namespace testing::transport {
using Socket = asio::ip::tcp::socket;
inline void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
inline std::string frame(unsigned kind, std::string_view route, std::string_view data, std::uint64_t id = 0) {
    std::string output;
    auto integer = [&](std::uint64_t value, unsigned width) {
        for (unsigned i = width; i; --i) output += static_cast<char>(value >> ((i - 1) * 8));
    };
    integer(14 + route.size() + data.size(), 4);
    integer(2, 1); integer(kind, 1); integer(0, 2); integer(id, 8); integer(route.size(), 2);
    output += route; output += data; return output;
}
inline std::string proof(std::string_view secret, bool server, std::string_view server_nonce, std::string_view client_nonce) {
    std::string input = server ? "fsnet/node/v2/auth/server" : "fsnet/node/v2/auth/client";
    input += server_nonce; input += client_nonce;
    std::array<unsigned char, 32> result{}; unsigned length = 0;
    require(HMAC(EVP_sha256(), secret.data(), static_cast<int>(secret.size()),
                 reinterpret_cast<const unsigned char*>(input.data()), input.size(), result.data(), &length)
                && length == result.size(), "fixture HMAC failed");
    return {reinterpret_cast<const char*>(result.data()), result.size()};
}
template<class Pump> std::string read_exact(Socket& socket, std::size_t count, Pump pump) {
    socket.non_blocking(true);
    std::string result(count, '\0'); std::size_t offset = 0;
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (offset < count) {
        pump();
        asio::error_code error;
        offset += socket.read_some(asio::buffer(result.data() + offset, count - offset), error);
        if (error && error != asio::error::would_block && error != asio::error::try_again)
            throw std::runtime_error("fixture connection closed");
        require(std::chrono::steady_clock::now() < deadline, "fixture receive deadline");
        if (error) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return result;
}
template<class Pump> std::string read_frame(Socket& socket, Pump pump) {
    auto header = read_exact(socket, 4, pump); std::size_t count = 0;
    for (unsigned char value : header) count = (count << 8) | value;
    require(count >= 14 && count < 1024 * 1024, "fixture invalid frame length");
    return read_exact(socket, count, pump);
}
inline void write(Socket& socket, std::string_view bytes) {
    socket.non_blocking(false);
    asio::write(socket, asio::buffer(bytes));
    socket.non_blocking(true);
}
template<class Pump> std::string authenticate(Socket& socket, Pump pump, std::string_view secret,
                                             std::string* observed_nonce = nullptr) {
    auto challenge = read_frame(socket, pump);
    require(challenge.size() == 47 && challenge[0] == 2 && challenge[1] == 0 && challenge[14] == 1,
            "fixture challenge format");
    auto server_nonce = challenge.substr(15); std::string client_nonce(32, 'c');
    if (observed_nonce) *observed_nonce = server_nonce;
    auto response = std::string(1, '\2') + client_nonce + proof(secret, false, server_nonce, client_nonce);
    write(socket, frame(0, {}, response));
    auto accepted = read_frame(socket, pump);
    require(accepted.size() == 47 && accepted[14] == 3 &&
            accepted.substr(15) == proof(secret, true, server_nonce, client_nonce), "fixture server proof");
    return response;
}
}
