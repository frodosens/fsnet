#pragma once
#include "fsnet/database.hpp"
#include <exception>
#include <functional>
#include <memory>

namespace fsnet {
struct StorageLimits {
    std::size_t jobs = 256;
    std::size_t bytes = 16 * 1024 * 1024;
};
// One storage worker; completions run only on the thread calling poll(). Queued,
// active and completed jobs all retain capacity until their completion is polled.
// A queue node is allocated at admission; moving an accepted job to the
// completion queue never allocates, even if the operation exhausts memory.
// Call submit/poll/stop on the owning thread. Operations must not access the VM,
// Node or caller-thread objects; capture owned input and a shared result instead.
class AsyncDatabase {
public:
    enum class StopMode { drain, cancel_queued };
    using Operation = std::function<void(Storage&)>;
    using Completion = std::function<void(std::exception_ptr)>;
    explicit AsyncDatabase(std::shared_ptr<Storage>, StorageLimits = {});
    ~AsyncDatabase();
    AsyncDatabase(const AsyncDatabase&) = delete;
    AsyncDatabase& operator=(const AsyncDatabase&) = delete;
    bool submit(std::size_t bytes, Operation, Completion);
    std::size_t poll(std::size_t budget = 64);
    std::size_t pending() const;
    // Default: drain accepted jobs and join the worker. cancel_queued instead
    // completes jobs that have not started with an exception and joins after
    // the currently executing job finishes. Both modes retain completions for
    // poll(); neither can cancel a write already accepted by the database.
    // A custom backend must provide its own bounded operation/cancellation
    // contract; a worker thread cannot safely force an arbitrary Storage to stop.
    void stop(StopMode = StopMode::drain);
private:
    struct Impl;
    std::shared_ptr<Impl> impl_;
};
}
