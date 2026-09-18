#pragma once
#include "pnm_component_diagnostics.h"
#include <algorithm>
#include <array>
#include <cassert>
#include <limits>
#include <map>
#include <set>
#include <tuple>
#include <vector>

// Read-only, bounded CTA0/warp0 observations. Predicted availability is not completion.
namespace custom_timing {
using U = unsigned long long;
struct Span {
  U count=0, sum=0, min=std::numeric_limits<U>::max(), max=0;
  void add(U v) { ++count; sum+=v; min=std::min(min,v); max=std::max(max,v); }
};
struct Inst {
  U launch, pc, issued;
  std::array<U,9> times{};
  unsigned mask=0, outstanding=0;
};
struct Reg { U uid, launch, pc, issued, released=0; bool pending=true; };
struct Edge { Span issue, release; };
struct Observations {
  std::map<std::tuple<U,U,U>,std::array<U,3>> barriers;
  std::map<U,Inst> instructions;
  std::map<std::tuple<unsigned,unsigned,unsigned>,Reg> registers;
  std::map<std::pair<unsigned,unsigned>,U> launches;
  std::map<std::tuple<U,U,unsigned>,Span> events;
  std::map<std::tuple<U,U,U,U,unsigned>,Edge> edges;
  std::map<std::tuple<U,U,U,unsigned>,U> blocked_counts, last_blocked;
  std::set<std::tuple<U,U,unsigned>> consumed;
  std::vector<std::array<U,13>> samples;
  std::vector<std::array<U,7>> register_samples;
  std::map<std::pair<U,U>,unsigned> sample_counts, release_counts;
  void barrier(U launch,U warp,U pc,U cycle) {
    if(!pnm_components::enabled()) return;
    auto &b=barriers[{launch,warp,pc}];
    if(!b[0]++) b[1]=cycle;
    b[2]=cycle;
  }
  void issue(unsigned sid,unsigned wid,U uid,U launch,U pc,U cycle) {
    if(!pnm_components::enabled()) return;
    auto key=std::make_pair(sid,wid); auto prior=launches.find(key);
    if(prior!=launches.end() && prior->second!=launch) {
      for(auto it=registers.begin();it!=registers.end();) {
        if(std::get<0>(it->first)==sid && std::get<1>(it->first)==wid) {
          assert(!it->second.pending); it=registers.erase(it);
        } else ++it;
      }
    }
    launches[key]=launch;
    assert(instructions.emplace(uid,Inst{launch,pc,cycle}).second);
    mark(uid,0,cycle);
  }
  void mark(U uid,unsigned event,U cycle) {
    if(!pnm_components::enabled()) return;
    auto it=instructions.find(uid); if(it==instructions.end()) return;
    auto &v=it->second; assert(event<9 && cycle>=v.issued);
    if(v.mask&(1u<<event)) return;
    v.mask|=1u<<event; v.times[event]=cycle;
    events[{v.launch,v.pc,event}].add(cycle-v.issued);
  }
  void reserve(unsigned sid,unsigned wid,unsigned reg,U uid) {
    if(!pnm_components::enabled()) return;
    auto it=instructions.find(uid); if(it==instructions.end()) return;
    auto key=std::make_tuple(sid,wid,reg); auto old=registers.find(key);
    assert(old==registers.end() || !old->second.pending);
    auto &v=it->second; ++v.outstanding;
    registers[key]=Reg{uid,v.launch,v.pc,v.issued};
  }
  void consume(unsigned sid,unsigned wid,unsigned reg,U uid,unsigned kind,U cycle) {
    if(!pnm_components::enabled()) return;
    auto c=instructions.find(uid); auto p=registers.find({sid,wid,reg});
    if(c==instructions.end() || p==registers.end() || p->second.launch!=c->second.launch) return;
    const auto &v=p->second;
    assert(!v.pending && cycle>=v.released);
    if(!consumed.emplace(uid,v.uid,kind).second) return;
    auto &e=edges[{v.launch,v.pc,c->second.launch,c->second.pc,kind}];
    e.issue.add(cycle-v.issued); e.release.add(cycle-v.released);
  }
  void blocked(unsigned sid,unsigned wid,unsigned reg,U pc,unsigned kind,U cycle) {
    if(!pnm_components::enabled()) return;
    auto p=registers.find({sid,wid,reg}); if(p==registers.end() || !p->second.pending) return;
    auto key=std::make_tuple(p->second.launch,p->second.pc,pc,kind);
    auto it=last_blocked.find(key);
    if(it==last_blocked.end() || it->second!=cycle) ++blocked_counts[key];
    last_blocked[key]=cycle;
  }
  void release(unsigned sid,unsigned wid,unsigned reg,U cycle) {
    if(!pnm_components::enabled()) return;
    auto it=registers.find({sid,wid,reg}); if(it==registers.end() || !it->second.pending) return;
    auto &v=it->second; assert(cycle>=v.issued); v.pending=false; v.released=cycle;
    auto inst=instructions.find(v.uid);
    if(inst!=instructions.end()) {
      assert(inst->second.outstanding);
      if(!--inst->second.outstanding) mark(v.uid,8,cycle);
    }
    auto key=std::make_pair(v.launch,v.pc);
    if(release_counts[key]++<16) register_samples.push_back({v.launch,v.pc,v.uid,reg,v.issued,cycle,cycle-v.issued});
  }
  void finish(U uid,U cycle) {
    if(!pnm_components::enabled()) return;
    auto it=instructions.find(uid); if(it==instructions.end()) return;
    const auto &v=it->second;
    assert(!v.outstanding);
    U last=0; bool output=false;
    for(const auto &r:registers) if(r.second.uid==uid) {
      assert(!r.second.pending); last=std::max(last,r.second.released); output=true;
    }
    if(output) mark(uid,8,last);
    auto key=std::make_pair(v.launch,v.pc);
    if(sample_counts[key]++<8) {
      std::array<U,13> row{v.launch,v.pc,uid,v.mask};
      for(unsigned i=0;i<9;++i) row[i+4]=v.times[i];
      samples.push_back(row);
    }
    instructions.erase(it);
    for(auto e=consumed.begin();e!=consumed.end();) {
      if(std::get<0>(*e)==uid) e=consumed.erase(e); else ++e;
    }
  }
  void print() {
    if(!pnm_components::enabled()) return;
    std::printf("CUSTOM_TIMING_VERSION 2\n");
    for(const auto &b:barriers)
      std::printf("CUSTOM_TIMING_BARRIER %llu %llu %llu %llu %llu %llu\n",std::get<0>(b.first),std::get<1>(b.first),std::get<2>(b.first),b.second[0],b.second[1],b.second[2]);
    barriers.clear();
    for(const auto &kv:events) {const auto &s=kv.second;
      std::printf("CUSTOM_TIMING_EVENT %llu %llu %u %llu %llu %llu %llu\n",std::get<0>(kv.first),std::get<1>(kv.first),std::get<2>(kv.first),s.count,s.sum,s.min,s.max);}
    for(const auto &kv:edges) {const auto &e=kv.second;
      std::printf("CUSTOM_TIMING_EDGE %llu %llu %llu %llu %u %llu %llu %llu %llu %llu %llu %llu\n",std::get<0>(kv.first),std::get<1>(kv.first),std::get<2>(kv.first),std::get<3>(kv.first),std::get<4>(kv.first),e.issue.count,e.issue.sum,e.issue.min,e.issue.max,e.release.sum,e.release.min,e.release.max);}
    for(const auto &kv:blocked_counts)
      std::printf("CUSTOM_TIMING_BLOCKED %llu %llu %llu %u %llu\n",std::get<0>(kv.first),std::get<1>(kv.first),std::get<2>(kv.first),std::get<3>(kv.first),kv.second);
    for(const auto &s:samples) {std::printf("CUSTOM_TIMING_SAMPLE");for(U v:s)std::printf(" %llu",v);std::printf("\n");}
    for(const auto &s:register_samples) {std::printf("CUSTOM_TIMING_REGISTER");for(U v:s)std::printf(" %llu",v);std::printf("\n");}
    U pending=0;for(const auto &r:registers)pending+=r.second.pending;
    std::printf("CUSTOM_TIMING_PENDING %llu %llu\n",U(instructions.size()),pending);
    events.clear();edges.clear();blocked_counts.clear();last_blocked.clear();samples.clear();register_samples.clear();sample_counts.clear();release_counts.clear();
    for(auto it=registers.begin();it!=registers.end();) {if(!it->second.pending)it=registers.erase(it);else ++it;}
  }
};
inline Observations &observations(){static Observations value;return value;}
}
