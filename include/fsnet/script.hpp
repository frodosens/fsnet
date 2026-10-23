#pragma once
#include "fsnet/node.hpp"
#include <cstddef>
#include <string>
#include <string_view>
#include <stdexcept>

namespace fsnet {
// Language-neutral host contract. Payloads are owned binary strings; script
// values, VM pointers, and C++ entities never cross this boundary. All methods
// and callbacks execute on the owning runtime thread.
class ScriptHost {
public:
    virtual ~ScriptHost() = default;
    virtual bool has_route(const std::string& route) const = 0;
    virtual void bind(std::string route, Handler handler) = 0;
    virtual void bind_stateful(std::string, std::string, std::string, Handler) {
        throw std::logic_error("stateful entities require EntityScriptHost");
    }
    virtual void unbind(const std::string& route) = 0;
    virtual std::uint64_t connect(const std::string& address, std::uint16_t port) = 0;
    virtual void call(std::uint64_t peer, std::string route, std::string data,
                      std::chrono::milliseconds timeout, Reply reply) = 0;
};
// Node must outlive its host and adapters. A host never owns or stops the Node.
class NodeScriptHost final : public ScriptHost {
    Node& node_;
public:
    explicit NodeScriptHost(Node& node) : node_(node) {}
    bool has_route(const std::string& route) const override { return node_.has_route(route); }
    void bind(std::string route, Handler handler) override { node_.bind(std::move(route), std::move(handler)); }
    void unbind(const std::string& route) override { node_.unbind(route); }
    std::uint64_t connect(const std::string& address, std::uint16_t port) override { return node_.connect(address, port); }
    void call(std::uint64_t peer, std::string route, std::string data,
              std::chrono::milliseconds timeout, Reply reply) override {
        node_.call(peer, std::move(route), std::move(data), timeout, std::move(reply));
    }
};
class ScriptEngine {
public:
    virtual ~ScriptEngine() = default;
    virtual void load(std::string_view source, const std::string& name) = 0;
    virtual std::size_t poll(std::size_t work_limit = 64) = 0;
    virtual void stop() = 0;
};
}
