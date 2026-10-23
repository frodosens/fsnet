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
// Call submit/poll/stop on the owning thread. Operations must not access the VM,
// Node or caller-thread objects; capture owned input and a shared result instead.
class AsyncDatabase {
public:
    using Operation = std::function<void(Database&)>;
    using Completion = std::function<void(std::exception_ptr)>;
    explicit AsyncDatabase(std::shared_ptr<Database>, StorageLimits = {});
    ~AsyncDatabase();
    AsyncDatabase(const AsyncDatabase&) = delete;
    AsyncDatabase& operator=(const AsyncDatabase&) = delete;
    bool submit(std::size_t bytes, Operation, Completion);
    std::size_t poll(std::size_t budget = 64);
    std::size_t pending() const;
    // Drains accepted writes and joins the worker. Poll remaining completions
    // afterwards. Stop cannot cancel a write already accepted by the database.
    void stop();
private:
    struct Impl;
    std::shared_ptr<Impl> impl_;
};
}
