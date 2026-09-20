#pragma once
#include <algorithm>
#include <array>
#include <cassert>
#include <cstdio>
#include <functional>
#include <map>
#include <tuple>
#include <vector>
#include "custom_icnt_queue_observation.h"

// Read-only CTA0/warp0 marker windows. Core timestamps; counters name their domain.
// Boundaries follow simulator callback order (including events in the same tick).
namespace custom_region {
using U = unsigned long long;
inline bool &enabled_flag() { static bool enabled=false; return enabled; }
struct Request { unsigned partition,type; int state; U begin; };
struct Residence {U segments=0,cycles=0,censored=0;};
struct Window {
  U icnt_first=0,icnt_last=0,icnt_ticks=0,icnt_heads=0,icnt_grants=0;
  std::vector<custom_icnt_queue::Sample> icnt_samples;
  std::vector<std::array<U,3>> icnt_missed; // bounded concrete tick/head/grant events
  std::map<unsigned,std::array<U,4>> icnt_destinations; // heads,queued,arrivals,departures

  U start=0,stop=0; bool open=true;
  std::map<std::tuple<unsigned,unsigned,unsigned,U>,U> counts;
  std::map<std::tuple<unsigned,unsigned,U,unsigned>,U> resources;
  std::map<std::tuple<unsigned,unsigned,int>,Residence> residence;
  std::map<unsigned,bool> direction;
  std::map<std::pair<unsigned,bool>,U> pending[2];
  std::map<unsigned,std::array<U,7>> l2[2];
  std::map<std::pair<unsigned,unsigned>,std::array<U,6>> cache[2];
};
struct Observations {
  std::map<U,Window> windows;
  std::map<U,Request> requests; // UID modulo64, bounded by outstanding requests
  std::map<std::pair<unsigned,bool>,U> pending;
  std::map<unsigned,std::function<std::array<U,7>()>> probes;
  std::map<unsigned,std::function<std::vector<std::array<U,7>>()>> cache_probes;
  bool has_open_window() const {
    if(!enabled_flag()) return false;
    for(const auto &p:windows) if(p.second.open) return true;
    return false;
  }
  void icnt_packet(unsigned destination,bool arrival) {
    if(!enabled_flag()) return;
    for(auto &p:windows) if(p.second.open) ++p.second.icnt_destinations[destination][arrival?2:3];
  }
  void icnt_tick(U tick,unsigned heads,unsigned grants,const custom_icnt_queue::Sample *sample,
                 const std::vector<int> &fronts,const std::vector<unsigned> &queued_by_destination) {
    assert(grants<=heads);
    for(auto &p:windows) if(p.second.open) {
      auto &w=p.second;
      if(!w.icnt_ticks) w.icnt_first=tick;
      w.icnt_last=tick;++w.icnt_ticks;w.icnt_heads+=heads;w.icnt_grants+=grants;
      if(heads!=grants && w.icnt_missed.size()<8) w.icnt_missed.push_back({tick,heads,grants});
      if(sample) {
        w.icnt_samples.push_back(*sample);
        for(auto d:fronts) if(d>=0) ++w.icnt_destinations[d][0];
        for(unsigned d=0;d<queued_by_destination.size();++d)
          w.icnt_destinations[d][1]+=queued_by_destination[d];
      }
    }
  }
  void created(unsigned type,bool write) {
    if(enabled_flag()) ++pending[{type,write}];
  }
  void destroyed(unsigned type,bool write) {
    if(!enabled_flag()) return;
    auto it=pending.find({type,write}); assert(it!=pending.end() && it->second);
    if(!--it->second) pending.erase(it);
  }
  void accrue(Window &w,const Request &r,U cycle,bool censored) {
    U begin=std::max(w.start,r.begin); assert(cycle>=begin);
    auto &s=w.residence[{r.partition,r.type,r.state}];
    ++s.segments; s.cycles+=cycle-begin; s.censored+=censored;
  }
  void snapshot(Window &w,unsigned edge) {
    w.pending[edge]=pending;
    for(const auto &p:probes) w.l2[edge][p.first]=p.second();
    for(const auto &p:cache_probes) for(const auto &s:p.second())
      w.cache[edge][{p.first,unsigned(s[0])}]={s[1],s[2],s[3],s[4],s[5],s[6]};
  }
  void clock(U launch,bool start,U cycle) {
    if(!enabled_flag()) return;
    if(start) {
      assert(windows.emplace(launch,Window{}).second);
      auto &w=windows.at(launch); w.start=cycle; snapshot(w,0);
    } else {
      auto &w=windows.at(launch); assert(w.open && cycle>=w.start);
      for(const auto &p:requests) accrue(w,p.second,cycle,true);
      snapshot(w,1); w.stop=cycle; w.open=false;
    }
  }
  void count(unsigned domain,unsigned kind,unsigned unit,U detail,U value=1) {
    if(!enabled_flag() || !value) return;
    for(auto &p:windows) if(p.second.open) p.second.counts[{domain,kind,unit,detail}]+=value;
  }
  void command(unsigned unit,bool write,unsigned bytes) {
    if(!enabled_flag()) return;
    count(3,2,unit,write,bytes);
    for(auto &p:windows) if(p.second.open) {
      auto &w=p.second; auto old=w.direction.find(unit);
      if(old!=w.direction.end() && old->second!=write) ++w.counts[{3,3,unit,write?1:2}];
      w.direction[unit]=write;
    }
  }
  void resource(unsigned sid,unsigned port,U pc,unsigned reason) {
    if(!enabled_flag()) return;
    for(auto &p:windows) if(p.second.open) ++p.second.resources[{sid,port,pc,reason}];
  }
  void transition(U uid,unsigned partition,unsigned type,int old_state,int state,U old_cycle,U cycle) {
    if(!enabled_flag() || uid%64) return;
    auto it=requests.emplace(uid,Request{partition,type,old_state,old_cycle}).first;
    auto &r=it->second;
    if(r.state==state && r.partition==partition && r.type==type) return;
    assert(cycle>=r.begin);
    for(auto &p:windows) if(p.second.open) accrue(p.second,r,cycle,false);
    r={partition,type,state,cycle};
  }
  void finish_request(U uid,U cycle) {
    if(!enabled_flag() || uid%64) return;
    auto it=requests.find(uid); if(it==requests.end()) return;
    for(auto &p:windows) if(p.second.open) accrue(p.second,it->second,cycle,false);
    requests.erase(it);
  }
  void print() {
    if(!enabled_flag()) return;
    std::printf("CUSTOM_REGION_VERSION %u 64\n",custom_icnt_queue::enabled_flag()?3:2);
    for(const auto &p:windows) {
      const auto &w=p.second; assert(!w.open);
      std::printf("CUSTOM_REGION_WINDOW %llu %llu %llu\n",p.first,w.start,w.stop);
      if(custom_icnt_queue::enabled_flag()) {
        std::printf("CUSTOM_REGION_ICNT_WINDOW %llu %llu %llu %llu %llu %llu\n",p.first,
          w.icnt_first,w.icnt_last,w.icnt_ticks,w.icnt_heads,w.icnt_grants);
        for(const auto &q:w.icnt_samples)
          std::printf("CUSTOM_REGION_ICNT_QUEUE %llu %llu %u %u %u %u %u %u %u\n",p.first,
            q.tick,q.ports,q.active_bound,q.all_matching,q.ready_matching,q.head_matching,q.grants,q.queued);
        for(const auto &d:w.icnt_destinations)
          std::printf("CUSTOM_REGION_ICNT_DEST %llu %u %llu %llu %llu %llu\n",p.first,d.first,
            d.second[0],d.second[1],d.second[2],d.second[3]);
        for(const auto &m:w.icnt_missed)
          std::printf("CUSTOM_REGION_ICNT_MISSED %llu %llu %llu %llu\n",p.first,m[0],m[1],m[2]);
      }
      for(const auto &c:w.counts)
        std::printf("CUSTOM_REGION_COUNT %llu %u %u %u %llu %llu\n",p.first,
          std::get<0>(c.first),std::get<1>(c.first),std::get<2>(c.first),std::get<3>(c.first),c.second);
      for(const auto &c:w.resources)
        std::printf("CUSTOM_REGION_RESOURCE %llu %u %u %llu %u %llu\n",p.first,
          std::get<0>(c.first),std::get<1>(c.first),std::get<2>(c.first),std::get<3>(c.first),c.second);
      for(const auto &c:w.residence)
        std::printf("CUSTOM_REGION_RESIDENCE %llu %u %u %d %llu %llu %llu\n",p.first,
          std::get<0>(c.first),std::get<1>(c.first),std::get<2>(c.first),c.second.segments,c.second.cycles,c.second.censored);
      for(unsigned edge=0;edge<2;++edge) {
        for(const auto &c:w.cache[edge]) {
          std::printf("CUSTOM_REGION_CACHE %llu %u %u %u",p.first,edge,c.first.first,c.first.second);
          for(U v:c.second) std::printf(" %llu",v);
          std::printf("\n");
        }
        for(const auto &c:w.pending[edge])
          std::printf("CUSTOM_REGION_PENDING %llu %u %u %u %llu\n",p.first,edge,c.first.first,unsigned(c.first.second),c.second);
        for(const auto &c:w.l2[edge]) {
          std::printf("CUSTOM_REGION_L2 %llu %u %u",p.first,edge,c.first);
          for(U v:c.second) std::printf(" %llu",v);
          std::printf("\n");
        }
      }
    }
    windows.clear(); // outstanding requests and registered caches outlive a launch
  }
};
inline Observations &observations() {static Observations value;return value;}
}
