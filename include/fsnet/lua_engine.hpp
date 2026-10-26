#pragma once
#include "fsnet/script.hpp"
#include <memory>

namespace fsnet {
struct ScriptLimits {
    std::size_t memory_bytes = 32 * 1024 * 1024;
    std::size_t source_bytes = 1024 * 1024;
    std::size_t queued_requests = 128;
    std::size_t queued_bytes = 4 * 1024 * 1024;
    std::size_t active_coroutines = 128;
    std::size_t registered_routes = 256;
    unsigned instructions_per_resume = 100000;
    std::chrono::milliseconds task_timeout{30000};
};
// Hosts trusted game scripts. Resource budgets contain accidental runaway code;
// this in-process VM is not a security boundary for hostile scripts.
class LuaEngine final : public ScriptEngine {
public:
    explicit LuaEngine(std::shared_ptr<ScriptHost> host, ScriptLimits limits = {});
    ~LuaEngine() override;
    LuaEngine(const LuaEngine&) = delete;
    LuaEngine& operator=(const LuaEngine&) = delete;
    void load(std::string_view source, const std::string& name = "script") override;
    std::size_t poll(std::size_t work_limit = 64) override;
    void stop() override;
    std::size_t active() const;
    std::size_t memory_used() const;
    std::string last_error() const;
    // runtime.start tasks are finite initialization work; runtime.background
    // tasks do not participate in readiness. Failure is sticky, including stop
    // cancellation of unfinished initialization. Neither query drives the VM.
    bool startup_complete() const;
    bool startup_failed() const;
private:
    struct Impl;
    std::shared_ptr<Impl> impl_;
};
}
