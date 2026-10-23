#include "fsnet/async_database.hpp"
#include <atomic>
#include <chrono>
#include <future>
#include <iostream>
#include <stdexcept>
#include <thread>

using namespace std::chrono_literals;
#define CHECK(c) do { if (!(c)) throw std::runtime_error(#c); } while (false)
int main() {
    try {
        auto db = fsnet::memory_database();
        fsnet::AsyncDatabase worker(db, {2, 16});
        auto owner = std::this_thread::get_id();
        std::promise<void> entered, release;
        auto gate = release.get_future().share();
        bool first = false, second = false;
        CHECK(worker.submit(8, [&](fsnet::Database& database) {
            CHECK(std::this_thread::get_id() != owner); entered.set_value(); gate.wait();
            CHECK(database.compare_exchange("value", 0, "persisted"));
        }, [&](std::exception_ptr error) { CHECK(!error); CHECK(std::this_thread::get_id() == owner); first = true; }));
        CHECK(entered.get_future().wait_for(2s) == std::future_status::ready);
        CHECK(worker.submit(8, [](fsnet::Database&) { throw std::runtime_error("storage failure"); },
            [&](std::exception_ptr error) { CHECK(error); second = true; }));
        CHECK(!worker.submit(1, [](fsnet::Database&) {}, [](std::exception_ptr) {}));
        CHECK(worker.poll() == 0 && !first); // storage cannot block the event loop
        release.set_value(); worker.stop();
        CHECK(worker.pending() == 2 && !first && !second); // completed jobs still consume capacity
        CHECK(!worker.submit(1, [](fsnet::Database&) {}, [](std::exception_ptr) {}));
        CHECK(worker.poll() == 2 && first && second && worker.pending() == 0);
        CHECK(db->get("value")->value == "persisted");
        auto self = std::make_unique<fsnet::AsyncDatabase>(db);
        CHECK(self->submit(1, [](fsnet::Database&) {}, [&](std::exception_ptr) { self.reset(); }));
        self->stop(); self->poll(); CHECK(!self);
        std::cout << "PASS async storage: bounded queue, nonblocking submit, owner-thread completion, errors and draining shutdown\n";
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
