#include "fsnet/entity_timer_state.hpp"
#include "fsnet/entity_message.hpp"
#include <limits>
#include <stdexcept>

namespace fsnet {
namespace {
void integer(std::string& out,std::uint64_t value){for(int shift=56;shift>=0;shift-=8)out+=static_cast<char>(value>>shift);}
std::uint64_t integer(std::string_view& input){if(input.size()<8)throw std::runtime_error("truncated timer integer");std::uint64_t value=0;for(unsigned i=0;i<8;++i)value=(value<<8)|static_cast<unsigned char>(input[i]);input.remove_prefix(8);return value;}
void blob(std::string& out,std::string_view value){if(value.size()>timer_payload_limit)throw std::invalid_argument("timer field limit");integer(out,value.size());out+=value;}
std::string blob(std::string_view& input,std::size_t maximum){auto n=integer(input);if(n>maximum || n>input.size())throw std::runtime_error("invalid timer field");auto value=std::string(input.substr(0,n));input.remove_prefix(n);return value;}
void valid(const EntityTimer& timer){
    if(timer.key.empty() || timer.key.size()>128 || timer.generation.empty() || timer.generation.size()>64 || !valid_entity_method(timer.method) ||
       timer.payload.size()>timer_payload_limit || timer.due_ms<0 || timer.interval_ms<0 ||
       timer.remaining>INT64_MAX || (!timer.interval_ms && timer.remaining!=1) || (timer.missed!=TimerMissed::coalesce && timer.missed!=TimerMissed::catch_up) ||
       (timer.fixed_delay && timer.missed==TimerMissed::catch_up))throw std::invalid_argument("invalid entity timer");
}
void put(std::string& out,const EntityTimer& timer){
    valid(timer);blob(out,timer.key);blob(out,timer.method);blob(out,timer.payload);blob(out,timer.generation);
    integer(out,static_cast<std::uint64_t>(timer.due_ms));integer(out,static_cast<std::uint64_t>(timer.interval_ms));integer(out,timer.remaining);
    out+=static_cast<char>(static_cast<unsigned>(timer.missed)|(timer.fixed_delay?2:0));
}
EntityTimer get(std::string_view& input){
    EntityTimer timer;timer.key=blob(input,128);timer.method=blob(input,127);timer.payload=blob(input,timer_payload_limit);timer.generation=blob(input,64);
    auto due=integer(input),interval=integer(input);if(due>INT64_MAX || interval>INT64_MAX)throw std::runtime_error("timer time overflow");
    timer.due_ms=static_cast<std::int64_t>(due);timer.interval_ms=static_cast<std::int64_t>(interval);timer.remaining=integer(input);
    if(input.empty() || static_cast<unsigned char>(input.front())>3)throw std::runtime_error("invalid timer flags");
    auto flags=static_cast<unsigned char>(input.front());input.remove_prefix(1);timer.missed=static_cast<TimerMissed>(flags&1);timer.fixed_delay=(flags&2)!=0;
    valid(timer);return timer;
}
}
bool advance_entity_timer(EntityTimer& timer,std::int64_t now) {
    valid(timer);if(now<timer.due_ms)throw std::invalid_argument("timer has not expired");
    if(!timer.interval_ms || (timer.remaining && --timer.remaining==0))return false;
    auto base=timer.fixed_delay?now:timer.due_ms;
    auto steps=timer.fixed_delay || timer.missed==TimerMissed::catch_up?std::uint64_t{1}:static_cast<std::uint64_t>((now-timer.due_ms)/timer.interval_ms)+1;
    auto available=static_cast<std::uint64_t>(INT64_MAX-base)/static_cast<std::uint64_t>(timer.interval_ms);
    if(steps>available)return false;
    timer.due_ms=base+static_cast<std::int64_t>(steps*static_cast<std::uint64_t>(timer.interval_ms));return true;
}
std::int64_t next_entity_timer(const EntityTimerState& state){auto due=INT64_MAX;for(const auto& [key,timer]:state.entries){(void)key;if(timer.due_ms<due)due=timer.due_ms;}return due;}
std::size_t entity_timers_wire_size(const EntityTimerState& state){
    if(state.entries.size()>1024 || state.token.size()>64 || ((state.sequence==0)!=state.token.empty()) || (!state.entries.empty() && !state.sequence))throw std::invalid_argument("invalid timer state");
    std::size_t size=28+state.token.size();
    for(const auto& [key,timer]:state.entries){
        if(key!=timer.key)throw std::invalid_argument("timer key mismatch");
        valid(timer);
        size+=57+timer.key.size()+timer.method.size()+timer.payload.size()+timer.generation.size();
    }
    if(size>2*1024*1024)throw std::length_error("timer state budget");
    return size;
}
std::string encode_entity_timers(const EntityTimerState& state){
    auto size=entity_timers_wire_size(state);
    std::string out="FST1";out.reserve(size);integer(out,state.sequence);blob(out,state.token);integer(out,state.entries.size());
    for(const auto& [key,timer]:state.entries){if(key!=timer.key)throw std::invalid_argument("timer key mismatch");put(out,timer);}
    if(out.size()>2*1024*1024)throw std::length_error("timer state budget");
    return out;
}
EntityTimerState decode_entity_timers(std::string_view input){
    if(!input.starts_with("FST1") || input.size()>2*1024*1024)throw std::runtime_error("invalid timer state");
    input.remove_prefix(4);EntityTimerState state;state.sequence=integer(input);state.token=blob(input,64);auto count=integer(input);
    if(count>1024 || ((state.sequence==0)!=state.token.empty()) || (count && !state.sequence))throw std::runtime_error("invalid timer count/token");
    while(count--){auto timer=get(input);auto key=timer.key;if(!state.entries.emplace(std::move(key),std::move(timer)).second)throw std::runtime_error("duplicate timer");}
    if(!input.empty())throw std::runtime_error("trailing timer bytes");
    return state;
}
std::string encode_timer_event(const EntityTimerEvent& event){if(event.fired_ms<event.timer.due_ms)throw std::invalid_argument("early timer event");std::string out="FTE1";put(out,event.timer);integer(out,static_cast<std::uint64_t>(event.fired_ms));return out;}
EntityTimerEvent decode_timer_event(std::string_view input){
    if(input.size()>timer_payload_limit+1024 || !input.starts_with("FTE1"))throw std::runtime_error("invalid timer event");
    input.remove_prefix(4);EntityTimerEvent result;result.timer=get(input);
    auto time=integer(input);if(!input.empty() || time>INT64_MAX || time<static_cast<std::uint64_t>(result.timer.due_ms))throw std::runtime_error("invalid timer event time");
    result.fired_ms=static_cast<std::int64_t>(time);return result;
}
}
