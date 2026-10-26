#include "fsnet/async_database.hpp"
#include <atomic>
#include <cstdlib>
#include <future>
#include <iostream>
#include <new>
#include <stdexcept>
#include <thread>

// Only the storage worker fails allocations. Admission on the caller thread
// remains available, as does the caller's completion processing.
thread_local bool reject_allocations = false;
std::atomic<unsigned> attempted_allocations{0};
void* operator new(std::size_t size) {
    if (reject_allocations) {
        ++attempted_allocations;
        throw std::bad_alloc();
    }
    if (auto* memory = std::malloc(size ? size : 1)) return memory;
    throw std::bad_alloc();
}
void* operator new[](std::size_t size) { return ::operator new(size); }
void operator delete(void* memory) noexcept { std::free(memory); }
void operator delete[](void* memory) noexcept { std::free(memory); }
void operator delete(void* memory, std::size_t) noexcept { std::free(memory); }
void operator delete[](void* memory, std::size_t) noexcept { std::free(memory); }

#define CHECK(condition) do { if (!(condition)) throw std::runtime_error(#condition); } while (false)

int main() {
    try {
        auto database = fsnet::memory_database();
        fsnet::AsyncDatabase worker(database, {64, 64});
        unsigned completed = 0;
        for (unsigned i = 0; i < 32; ++i) {
            CHECK(worker.submit(1, [](fsnet::Storage&) { reject_allocations = true; },
                [&](std::exception_ptr error) { CHECK(!error); ++completed; }));
        }
        worker.stop();
        CHECK(worker.pending() == 32);
        CHECK(worker.poll(64) == 32);
        CHECK(completed == 32 && worker.pending() == 0);
        CHECK(attempted_allocations == 0);

        // An allocation failure at admission accepts no operation and does not
        // consume capacity. A subsequent healthy operation must still complete.
        fsnet::AsyncDatabase admission(database, {1, 1});
        bool failed = false, ran = false;
        reject_allocations = true;
        try { admission.submit(1, [](fsnet::Storage&) {}, [](std::exception_ptr) {}); }
        catch (const std::bad_alloc&) { failed = true; }
        reject_allocations = false;
        CHECK(failed && admission.pending() == 0);
        CHECK(admission.submit(1, [&](fsnet::Storage&) { ran = true; },
            [](std::exception_ptr error) { CHECK(!error); }));
        admission.stop();
        CHECK(admission.poll() == 1 && ran && admission.pending() == 0);

        // Fast shutdown cancels only jobs that have not started. The active
        // write still completes and every admitted job retains its callback.
        fsnet::AsyncDatabase cancellation(database, {16, 16});
        std::promise<void> entered, release;
        auto gate = release.get_future().share();
        unsigned active_completed = 0, cancelled = 0;
        std::atomic<unsigned> writes = 0;
        CHECK(cancellation.submit(1, [&](fsnet::Storage&) {
            entered.set_value(); gate.wait(); ++writes;
        }, [&](std::exception_ptr error) { CHECK(!error); ++active_completed; }));
        CHECK(entered.get_future().wait_for(std::chrono::seconds(2)) == std::future_status::ready);
        for (unsigned i = 0; i < 8; ++i) {
            CHECK(cancellation.submit(1, [&](fsnet::Storage&) { ++writes; },
                [&](std::exception_ptr error) { CHECK(error); ++cancelled; }));
        }
        std::thread unblock([&] {
            std::this_thread::sleep_for(std::chrono::milliseconds(20)); release.set_value();
        });
        const auto before_cancel = attempted_allocations.load();
        reject_allocations = true;
        try { cancellation.stop(fsnet::AsyncDatabase::StopMode::cancel_queued); }
        catch (...) { reject_allocations = false; unblock.join(); throw; }
        reject_allocations = false;
        unblock.join();
        CHECK(attempted_allocations == before_cancel);
        CHECK(cancellation.pending() == 9);
        CHECK(cancellation.poll(16) == 9);
        CHECK(writes == 1 && active_completed == 1 && cancelled == 8 && cancellation.pending() == 0);
        std::cout << "PASS storage resource recovery: completion handoff without allocation and admission rollback\n";
    } catch (const std::exception& error) {
        reject_allocations = false;
        std::cerr << error.what() << '\n';
        return 1;
    }
}
