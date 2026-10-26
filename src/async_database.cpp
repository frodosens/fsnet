#include "fsnet/async_database.hpp"
#include <condition_variable>
#include <list>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <utility>

namespace fsnet {
struct AsyncDatabase::Impl {
    struct Job { std::size_t bytes; Operation operation; Completion completion; std::exception_ptr error; };
    std::shared_ptr<Storage> database;
    StorageLimits limits;
    mutable std::mutex mutex;
    std::condition_variable ready;
    // Admission owns the only queue node allocation. Moving a job through the
    // worker and completion queues must not allocate after its operation ran:
    // the operation may have committed even when the process is short of memory.
    std::list<Job> queued, completed;
    std::exception_ptr cancelled = std::make_exception_ptr(std::runtime_error("storage operation cancelled before execution"));
    std::size_t count = 0, bytes = 0;
    bool stopped = false, polling = false;
    std::thread worker;
    Impl(std::shared_ptr<Storage> db, StorageLimits settings) : database(std::move(db)), limits(settings) {
        if (!database || !limits.jobs || !limits.bytes) throw std::invalid_argument("invalid storage executor");
        worker = std::thread([this] {
            for (;;) {
                std::list<Job> active;
                {
                    std::unique_lock lock(mutex);
                    ready.wait(lock, [&] { return stopped || !queued.empty(); });
                    if (queued.empty()) return;
                    active.splice(active.end(), queued, queued.begin());
                }
                auto& job = active.front();
                try { job.operation(*database); } catch (...) { job.error = std::current_exception(); }
                job.operation = {};
                { std::lock_guard lock(mutex); completed.splice(completed.end(), active); }
            }
        });
    }
    void stop(StopMode mode = StopMode::drain) {
        std::list<Job> abandoned;
        {
            std::lock_guard lock(mutex);
            stopped = true;
            if (mode == StopMode::cancel_queued) abandoned.splice(abandoned.end(), queued);
        }
        // Release owned operation captures outside the executor's mutex, just
        // as the worker does. The prebuilt exception and list splice allocate
        // nothing even if stop follows a resource-exhaustion failure.
        for (auto& job : abandoned) { job.error = cancelled; job.operation = {}; }
        ready.notify_one();
        if (worker.joinable()) worker.join();
        // The active job precedes these cancelled queued jobs in admission
        // order, so append their completions only after that job has finished.
        { std::lock_guard lock(mutex); completed.splice(completed.end(), abandoned); }
    }
    ~Impl() { stop(); }
};
AsyncDatabase::AsyncDatabase(std::shared_ptr<Storage> db, StorageLimits limits)
    : impl_(std::make_shared<Impl>(std::move(db), limits)) {}
AsyncDatabase::~AsyncDatabase() { stop(); }
bool AsyncDatabase::submit(std::size_t bytes, Operation operation, Completion completion) {
    auto& self = *impl_;
    if (!operation || !completion) throw std::invalid_argument("storage callbacks required");
    std::lock_guard lock(self.mutex);
    if (self.stopped || self.count >= self.limits.jobs || bytes > self.limits.bytes - self.bytes) return false;
    self.queued.push_back({bytes, std::move(operation), std::move(completion), {}});
    ++self.count; self.bytes += bytes; self.ready.notify_one(); return true;
}
std::size_t AsyncDatabase::poll(std::size_t budget) {
    auto keep_alive = impl_;
    auto& self = *keep_alive;
    if (self.polling) throw std::logic_error("recursive storage poll");
    self.polling = true;
    struct Reset { bool& flag; ~Reset() { flag = false; } } reset{self.polling};
    std::size_t work = 0;
    while (work < budget) {
        Impl::Job job;
        {
            std::lock_guard lock(self.mutex);
            if (self.completed.empty()) break;
            job = std::move(self.completed.front()); self.completed.pop_front();
            --self.count; self.bytes -= job.bytes;
        }
        ++work; job.completion(job.error);
    }
    return work;
}
std::size_t AsyncDatabase::pending() const { std::lock_guard lock(impl_->mutex); return impl_->count; }
void AsyncDatabase::stop(StopMode mode) { impl_->stop(mode); }
}
