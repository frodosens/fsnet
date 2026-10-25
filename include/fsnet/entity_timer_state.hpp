#pragma once
#include <cstdint>
#include <map>
#include <string>
#include <string_view>

namespace fsnet {
enum class TimerMissed : unsigned char { coalesce=0, catch_up=1 };
struct EntityTimerSpec {
    std::string key,method,payload;
    std::int64_t delay_ms=0,at_ms=-1,interval_ms=0;
    std::uint64_t count=1; // zero means unlimited periodic callbacks
    TimerMissed missed=TimerMissed::coalesce;
    bool fixed_delay=false;
};
struct EntityTimer {
    std::string key,method,payload,generation;
    std::int64_t due_ms=0,interval_ms=0;
    std::uint64_t remaining=1;
    TimerMissed missed=TimerMissed::coalesce;
    bool fixed_delay=false;
};
struct EntityTimerState {
    std::uint64_t sequence=0;
    std::string token;
    std::map<std::string,EntityTimer> entries;
};
struct EntityTimerEvent {EntityTimer timer;std::int64_t fired_ms=0;};
inline constexpr std::size_t timer_payload_limit=65536;
// Pure shared state/codec, independent of VM, transport and persistence adapter.
bool advance_entity_timer(EntityTimer&,std::int64_t now_ms);
std::int64_t next_entity_timer(const EntityTimerState&);
std::string encode_entity_timers(const EntityTimerState&);
EntityTimerState decode_entity_timers(std::string_view);
std::string encode_timer_event(const EntityTimerEvent&);
EntityTimerEvent decode_timer_event(std::string_view);
}
