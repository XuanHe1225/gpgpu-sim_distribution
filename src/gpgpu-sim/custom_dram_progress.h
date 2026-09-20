#pragma once
// Optional whole-launch observations. No queue, policy, or timing mutations.
#include <algorithm>
#include <cassert>
#include <cstdio>
#include <functional>
#include <map>
#include <tuple>
#include <utility>
namespace custom_dram_progress {
using U = unsigned long long;
constexpr U bin_cycles = 10000;
inline bool &enabled_flag() { static bool value=false; return value; }
inline unsigned owner(unsigned sid) { return sid<108 ? sid : 108; }
using State = std::tuple<unsigned,unsigned,unsigned>;
using Key = std::tuple<U,unsigned,unsigned,unsigned,unsigned>;
struct Value { U count=0, bytes=0; };
struct Observations {
  std::map<Key,Value> bins;
  std::map<std::pair<unsigned,unsigned>,U> live;
  std::map<State,U> pending_states, state_since;
  void add(unsigned sid,unsigned type,unsigned kind,unsigned detail,U cycle,U count,U bytes=0) {
    auto &v=bins[{cycle/bin_cycles,owner(sid),type,kind,detail}];
    v.count+=count; v.bytes+=bytes;
  }
  void settle(State key,U cycle) {
    U start=state_since[key]; assert(cycle>=start);
    const U n=pending_states[key];
    while(n && start<cycle) {
      U stop=std::min(cycle,(start/bin_cycles+1)*bin_cycles);
      add(std::get<0>(key),std::get<1>(key),4,std::get<2>(key),start,n*(stop-start));
      start=stop;
    }
    state_since[key]=cycle;
  }
  void created(unsigned sid,unsigned type,unsigned bytes,U cycle) {
    if(!enabled_flag())return; sid=owner(sid);
    add(sid,type,1,0,cycle,1,bytes); ++live[{sid,type}];
    State key{sid,type,0}; settle(key,cycle); ++pending_states[key];
  }
  void transition(unsigned sid,unsigned type,unsigned old_state,unsigned state,unsigned bytes,U cycle) {
    if(!enabled_flag() || old_state==state)return; sid=owner(sid);
    State old{sid,type,old_state},next{sid,type,state};
    settle(old,cycle); settle(next,cycle);
    assert(pending_states[old]>0); --pending_states[old]; ++pending_states[next];
    add(sid,type,3,state,cycle,1,bytes);
  }
  void destroyed(unsigned sid,unsigned type,unsigned state,unsigned bytes,U cycle) {
    if(!enabled_flag())return; sid=owner(sid);
    State key{sid,type,state}; settle(key,cycle);
    assert(pending_states[key]>0 && live[std::make_pair(sid,type)]>0);
    --pending_states[key]; --live[{sid,type}]; add(sid,type,2,state,cycle,1,bytes);
  }
  void network(unsigned sid,unsigned type,unsigned subnet,unsigned event,unsigned bytes,U cycle) {
    if(enabled_flag())add(sid,type,5,10*subnet+event,cycle,1,bytes);
  }
  void print(U cycle) {
    if(!enabled_flag())return;
    for(const auto &p:pending_states)settle(p.first,cycle);
    std::printf("CUSTOM_PROGRESS_VERSION 1 %llu %llu\n",bin_cycles,cycle);
    for(const auto &p:bins)std::printf("CUSTOM_PROGRESS_BIN %llu %u %u %u %u %llu %llu\n",
      std::get<0>(p.first),std::get<1>(p.first),std::get<2>(p.first),std::get<3>(p.first),
      std::get<4>(p.first),p.second.count,p.second.bytes);
    for(const auto &p:live)std::printf("CUSTOM_PROGRESS_PENDING %u %u %llu\n",p.first.first,p.first.second,p.second);
    bins.clear();
  }
};
inline Observations &observations() { static Observations value; return value; }
inline std::function<void(void*,unsigned,unsigned)> &network_hook() {
  static std::function<void(void*,unsigned,unsigned)> value; return value;
}
inline void network_packet(void *packet,unsigned subnet,unsigned event) {
  if(enabled_flag() && network_hook())network_hook()(packet,subnet,event);
}
}
