#include "fsnet/entity_host.hpp"
#include "fsnet/entity_message.hpp"
#include "fsnet/entity_record.hpp"
#include <atomic>
#include <condition_variable>
#include <cstdlib>
#include <iostream>
#include <mutex>
#include <new>
#include <thread>
#include <unordered_map>

// Fail one owner-thread allocation at the handler -> framework boundary. The
// storage worker must not accidentally consume the injected failure.
thread_local bool fail_next_allocation = false;
thread_local int fail_after_allocation = -1;
void* operator new(std::size_t size) {
    if (std::exchange(fail_next_allocation, false)) throw std::bad_alloc();
    if(fail_after_allocation>=0 && fail_after_allocation--==0)throw std::bad_alloc();
    if (auto pointer = std::malloc(size ? size : 1)) return pointer;
    throw std::bad_alloc();
}
void operator delete(void* pointer) noexcept { std::free(pointer); }
void operator delete(void* pointer, std::size_t) noexcept { std::free(pointer); }

using namespace fsnet;
using namespace std::chrono_literals;
void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
struct Parent final : ScriptHost {
    std::unordered_map<std::string, ContextHandler> routes;
    bool has_route(const std::string& route) const override { return routes.contains(route); }
    void bind(std::string, Handler) override { throw std::logic_error("context required"); }
    void bind_context(std::string route, RpcPolicy, ContextHandler handler) override {
        routes.emplace(std::move(route), std::move(handler));
    }
    void unbind(const std::string& route) override { routes.erase(route); }
    std::uint64_t connect(const std::string&, std::uint16_t) override { return 0; }
    void call(std::uint64_t, std::string, std::string, std::chrono::milliseconds, Reply reply) override {
        reply({Status::not_found, {}});
    }
};
struct Fixture {
    std::shared_ptr<Parent> parent = std::make_shared<Parent>();
    std::shared_ptr<Storage> db;
    std::shared_ptr<EntityScriptHost> host;
    std::atomic<std::int64_t> wall{100000};
    explicit Fixture(std::shared_ptr<Storage> storage = memory_database(),EntityScriptHost::WallClock clock={}) : db(std::move(storage)) {
        EntityRuntimeLimits limits; limits.request_timeout = 200ms; limits.lease = 1000ms;
        host = std::make_shared<EntityScriptHost>(parent, db, "production/boot", limits,
            StorageLimits{}, clock?std::move(clock):EntityScriptHost::WallClock([this] { return wall.load(); }));
        host->define_schema("Actor", {});
    }
    ~Fixture() { host->stop(); }
    void poll() { host->poll(); std::this_thread::sleep_for(1ms); }
    template<class Done> void until(Done done) {
        auto deadline = std::chrono::steady_clock::now() + 3s;
        while (!done()) { poll(); require(std::chrono::steady_clock::now() < deadline, "test deadline"); }
    }
    template<class Start> Result wait(Start start) {
        std::optional<Result> result;
        start([&](Result value) { result = std::move(value); });
        until([&] { return result.has_value(); }); return std::move(*result);
    }
    EntityId create(EntityPersistence persistence = EntityPersistence::durable) {
        auto result = wait([&](Reply reply) {
            host->create_entity("Actor", {}, {"production", "actor", {}, "production", persistence}, std::move(reply));
        });
        require(result.status == Status::ok, "create failed"); return EntityId(result.data);
    }
    void bind(std::string method, ContextHandler handler) {
        host->bind_stateful_context("Actor." + method, "Actor", encode_attributes({0, 0, true, {}}),
            {false, true, false}, std::move(handler));
    }
    Result call(EntityId id, std::string_view method, std::string_view receipt = {}) {
        return wait([&](Reply reply) {
            host->call_entity("@", entity_call_message(id, method, "", receipt), 1s, std::move(reply));
        });
    }
    void healthy() {
        bind("healthy", [](const CallIdentity&, std::string input, Reply reply) {
            auto call = parse_entity_message(input); auto state = parse_state_message(call->payload);
            reply({Status::ok, state_message(state->state, "healthy")});
        });
    }
};

void receipts() {
    for (auto persistence : {EntityPersistence::durable, EntityPersistence::rebuildable}) {
        Fixture fixture; unsigned executions = 0;
        fixture.bind("hit", [&](const CallIdentity&, std::string input, Reply reply) {
            auto call = parse_entity_message(input); auto state = parse_state_message(call->payload);
            reply({Status::ok, state_message(state->state, std::to_string(++executions))});
        });
        auto id = fixture.create(persistence);
        require(fixture.call(id, "hit", "same").data == "1", "first receipt");
        require(fixture.call(id, "hit", "same").data == "1" && executions == 1, "resident empty-schema replay duplicated");
        fixture.wall += 900; // force a lease renewal without unloading the instance
        require(fixture.call(id, "hit", "same").data == "1" && executions == 1, "renewal lost receipt");
        auto off = fixture.wait([&](Reply reply) { fixture.host->deactivate(*fixture.host->handle("Actor", id.str()), std::move(reply)); });
        require(off.status == Status::ok, "receipt fixture unload");
        auto recovered = fixture.call(id, "hit", "same");
        require(recovered.data == (persistence == EntityPersistence::rebuildable ? "2" : "1"), "unload persistence policy");
        require(fixture.host->stats().pending == 0, "receipt pending count");
    }
}

void active_deadline() {
    Fixture fixture; fixture.healthy(); Reply late; std::string late_state; std::optional<CallIdentity> original;
    fixture.bind("hang", [&](const CallIdentity& identity, std::string input, Reply reply) {
        require(identity.live(), "new execution identity is not live");original=identity;
        auto call = parse_entity_message(input); late_state = parse_state_message(call->payload)->state;
        late = std::move(reply);
    });
    auto id = fixture.create(); std::optional<Result> active, queued;
    fixture.host->call_entity("@", entity_call_message(id, "hang", ""), 120ms,
        [&](Result result) { active = std::move(result); });
    fixture.until([&] { return bool(late); });
    fixture.host->call_entity("@", entity_call_message(id, "healthy", ""), 15ms,
        [&](Result result) { queued = std::move(result); });
    fixture.until([&] { return queued.has_value(); });
    require(queued->status == Status::timeout && !active, "queued deadline hidden behind busy handler");
    fixture.until([&] { return active.has_value(); });
    require(active->status == Status::timeout && fixture.host->stats().pending == 0, "active native timeout did not release mailbox");
    require(original && !original->live(), "timed out execution identity remained live");
    auto version = fixture.db->get(entity_record_key(id))->version;
    late({Status::ok, state_message(late_state, "late")});
    for (unsigned i = 0; i < 5; ++i) fixture.poll();
    require(fixture.db->get(entity_record_key(id))->version == version, "late native completion committed");
    require(fixture.call(id, "healthy").data == "healthy", "mailbox unusable after timeout");
    require(!original->live(), "next entity transaction revived old execution identity");
    require(fixture.host->stats().state_bytes == 0, "timed out execution retained charged state");
}

void completion_failure() {
    Fixture fixture; fixture.healthy(); bool injected = false;
    fixture.bind("fail", [&](const CallIdentity&, std::string input, Reply reply) {
        auto call = parse_entity_message(input); auto state = parse_state_message(call->payload);
        Result result{Status::ok, state_message(state->state, "allocation-fixture")};
        fail_next_allocation = true;
        reply(std::move(result));
        injected = !fail_next_allocation;
        fail_next_allocation = false;
    });
    auto id = fixture.create(); auto before = fixture.db->get(entity_record_key(id));
    auto failed = fixture.call(id, "fail");
    require(injected && failed.status == Status::internal, "completion fault not handled");
    require(fixture.host->stats().pending == 0 && fixture.host->stats().state_bytes == 0, "completion failure stranded request");
    require(fixture.db->get(entity_record_key(id))->version == before->version, "failed completion committed");
    require(fixture.call(id, "healthy").data == "healthy", "completion failure poisoned later requests");
}

void acquisition_admission_failure() {
    struct Control {bool armed=false;int skip=0;};
    struct Clock {
        Control* control;
        explicit Clock(Control& value):control(&value){}
        Clock(const Clock& other):control(other.control) {
            if(std::exchange(control->armed,false))fail_after_allocation=control->skip;
        }
        std::int64_t operator()()const{return 100000;}
    };
    // Copying the clock during acquire arms a failure after the accepted
    // request moved out of the mailbox. Sweep capture/function/node admission
    // allocations without letting storage-worker allocations consume it.
    for(int skip=0;skip<4;++skip) {
        Control control;Fixture fixture(memory_database(),Clock(control));fixture.healthy();auto id=fixture.create();
        require(fixture.call(id,"healthy").status==Status::ok,"admission fixture warmup");
        auto before=fixture.db->get(entity_record_key(id));std::optional<Result> result;
        fixture.host->call_entity("@",entity_call_message(id,"healthy",""),1s,[&](Result value){result=std::move(value);});
        control.skip=skip;control.armed=true;
        fixture.until([&]{return result.has_value();});
        auto consumed=fail_after_allocation<0;fail_after_allocation=-1;
        require(consumed && result->status==Status::internal,"acquire admission allocation did not complete");
        require(fixture.host->stats().pending==0 && fixture.host->stats().state_bytes==0,"failed acquisition retained busy mailbox");
        require(fixture.db->get(entity_record_key(id))->version==before->version,"failed admission wrote entity");
        require(fixture.call(id,"healthy").data=="healthy","failed admission poisoned mailbox");
    }
}

void authorized_read_race() {
    Fixture fixture; unsigned activations = 0;
    fixture.bind("$activate", [&](const CallIdentity&, std::string input, Reply reply) {
        ++activations;
        auto call = parse_entity_message(input); auto state = parse_state_message(call->payload);
        reply({Status::ok, state_message(state->state, "")});
    });
    auto id = fixture.create(); auto alice = ConnectionRef::create(), bob = ConnectionRef::create();
    auto bound = fixture.wait([&](Reply reply) { fixture.host->bind_entity_owner("Actor", id.str(), alice, std::move(reply)); });
    require(bound.status == Status::ok, "owner fixture bind");
    auto off = fixture.wait([&](Reply reply) { fixture.host->deactivate(*fixture.host->handle("Actor", id.str()), std::move(reply)); });
    require(off.status == Status::ok, "owner fixture unload");
    std::uint64_t changed_version = 0; unsigned guards = 0;
    auto result = fixture.wait([&](Reply reply) {
        fixture.host->read_attributes("Actor", id.str(), [&](const std::string& owner) {
            ++guards;
            require(owner == alice.key(), "guard did not receive stored owner");
            // Replace ownership after metadata authorization, before the queued
            // snapshot can obtain a lease. The expected-owner check must reject.
            auto saved = fixture.db->get(entity_record_key(id)); auto record = decode_entity_record(saved->value);
            record.metadata->owner_connection = bob.key(); ++record.metadata->owner_revision;
            require(fixture.db->compare_exchange(entity_record_key(id), saved->version, encode_entity_record(record)), "owner race fixture CAS");
            changed_version = fixture.db->get(entity_record_key(id))->version;
            return true;
        }, std::move(reply));
    });
    auto saved = fixture.db->get(entity_record_key(id)); auto record = decode_entity_record(saved->value);
    require(result.status == Status::rejected && guards == 1, "changed owner reached lifecycle guard");
    require(activations == 1 && !fixture.host->active("Actor", id.str()), "changed owner activated entity");
    require(saved->version == changed_version && record.owner.empty(), "changed owner obtained a lease");
    auto binding = fixture.wait([&](Reply reply) { fixture.host->read_owner_binding("Actor", id.str(), std::move(reply)); });
    require(binding.status == Status::ok && binding.data == bob.key(), "nonactivating owner lookup");
    require(fixture.db->get(entity_record_key(id))->version == changed_version, "metadata lookup changed entity");
}

struct GateStorage final : Storage {
    std::shared_ptr<Storage> storage = memory_database();
    std::atomic<bool> armed = false, entered = false;
    std::mutex mutex; std::condition_variable ready; bool released = false;
    std::optional<Record> get(std::string_view key) override { return storage->get(key); }
    bool erase(std::string_view key, std::uint64_t expected) override { return storage->erase(key, expected); }
    bool compare_exchange(std::string_view key, std::uint64_t expected, std::string_view value) override {
        if (key.starts_with("fsnet/entity/v2/") && armed.exchange(false)) {
            entered = true;
            std::unique_lock lock(mutex); ready.wait(lock, [&] { return released; });
        }
        return storage->compare_exchange(key, expected, value);
    }
    void release() { { std::lock_guard lock(mutex); released = true; } ready.notify_all(); }
};
void pending_commit() {
    auto storage = std::make_shared<GateStorage>(); Fixture fixture(storage); fixture.healthy();
    struct Release { GateStorage& storage; ~Release() { storage.release(); } } release{*storage};
    auto id = fixture.create(); storage->armed = true; std::optional<Result> result;
    fixture.host->call_entity("@", entity_call_message(id, "healthy", ""), 100ms,
        [&](Result value) { result = std::move(value); });
    fixture.until([&] { return storage->entered.load(); });
    auto until = std::chrono::steady_clock::now() + 130ms;
    while (std::chrono::steady_clock::now() < until) fixture.poll();
    require(!result && fixture.host->stats().pending == 1, "watchdog abandoned an accepted CAS");
    storage->release(); fixture.until([&] { return result.has_value(); });
    require(result->status == Status::ok && fixture.host->stats().pending == 0, "accepted CAS outcome discarded");
}

int main() {
    try {
        receipts(); active_deadline(); completion_failure(); acquisition_admission_failure(); authorized_read_race(); pending_commit();
        std::cout << "PASS production entity: empty-schema receipts, active/queued deadlines, late reply fencing, allocation recovery and pending CAS\n";
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
