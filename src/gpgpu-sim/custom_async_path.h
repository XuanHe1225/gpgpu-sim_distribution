// Read-only, bounded async loop observations. No queue or timing decisions.
#ifndef CUSTOM_ASYNC_PATH_H
#define CUSTOM_ASYNC_PATH_H
#include "custom_component_target.h"
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <sstream>
#include <string>
#include <tuple>
#include <unordered_map>
#include <vector>

namespace custom_async_path {
using U = unsigned long long;
struct Config {
  std::string kernel;
  U loop = 0, stop = 0;
  bool enabled = false;
  Config() {
    const char *value = std::getenv("CUSTOM_ASYNC_PATH_OBSERVATION");
    if (!value) return;
    std::istringstream input(value);
    enabled = bool(input >> kernel >> loop >> stop);
  }
};
inline const Config &config() { static Config c; return c; }
inline bool enabled() { return config().enabled; }
enum Time { START, FIRST_ISSUE, LAST_ISSUE, FIRST_INJECT, LAST_INJECT,
            FIRST_RESPONSE, LAST_RESPONSE, LAST_COMPLETE, WAIT_ENTER,
            WAIT_RELEASE, BARRIER_ARRIVE, BARRIER_RELEASE, NEXT, TIMES };
struct Row {
  U launch, x, y, z, sid, warp, iteration, group;
  std::array<U, TIMES> time{};
  U issued = 0, completed = 0, injected = 0, returned = 0, bytes = 0;
};
struct Request { size_t row; U inst; int state; U begin; };
struct Span { U count = 0, sum = 0, max = 0; };
struct Front {
  std::array<U, 3> time{};
  unsigned stage = 0;
  U icnt_stalls = 0, coal_stalls = 0;
  std::array<U, 3> collector_waits{};
  struct Block { U count = 0, first = 0, last = 0; };
  std::map<U, Block> collector_blocks;
};
struct Observations {
  std::vector<Row> rows;
  std::map<std::pair<U,U>,size_t> current;
  std::unordered_map<U,size_t> instructions;
  std::unordered_map<U,Request> requests;
  std::unordered_map<U,Front> fronts;
  std::map<std::tuple<U,U,int,int>,Span> residence;
  U errors = 0, inactive = 0;
  static bool detailed(U iteration) {
    return iteration == 0 || iteration == 1024 || iteration == 2047;
  }
  static void first(U &field, U cycle) { if (!field) field = cycle; }
  // kind: 1=LDGSTS, 2=DEPBAR, 3=CTA barrier. Loop PCs are binary-bound.
  void issue(const std::string &name, U uid, U launch, U sid, U warp,
             U x, U y, U z, U pc, unsigned kind, U group, U cycle) {
    if (!enabled() || config().kernel != name) return;
    const auto key = std::make_pair(sid,warp);
    auto it = current.find(key);
    if (pc == config().loop) {
      U iteration = 0;
      if (it != current.end()) {
        auto &previous = rows[it->second];
        previous.time[NEXT] = cycle;
        iteration = previous.iteration + 1;
      }
      // A configured diagnostic must not grow without bound on another trace.
      if (rows.size() >= 131072) { ++errors; return; }
      Row row{launch,x,y,z,sid,warp,iteration,group};
      row.time[START] = cycle;
      current[key] = rows.size(); rows.push_back(row);
      return;
    }
    if (it == current.end()) return;
    auto &row = rows[it->second];
    if (pc == config().stop) { row.time[NEXT] = cycle; current.erase(it); return; }
    if (kind == 1) {
      first(row.time[FIRST_ISSUE],cycle); row.time[LAST_ISSUE] = cycle;
      ++row.issued;
      if (!instructions.emplace(uid,it->second).second) ++errors;
      if (detailed(row.iteration))
        std::printf("CUSTOM_ASYNC_ISSUE %llu %llu %llu %llu %llu %llu %llu %llu\n",
                    uid,launch,sid,warp,row.iteration,group,pc,cycle);
    } else if (kind == 2) row.time[WAIT_ENTER] = cycle;
    else if (kind == 3) row.time[BARRIER_ARRIVE] = cycle;
  }
  // Observe only already-selected async instructions; never alter admission.
  bool collector_active(U sid) const {
    if (!enabled()) return false;
    auto it = current.find({sid, 0});
    if (it == current.end()) return false;
    const auto &r = rows[it->second];
    return detailed(r.iteration) && custom_component::selected_cta(r.x, r.y, r.z);
  }
  void front_collector(U uid, bool operands_ready, U blocker, U cycle) {
    if (!enabled()) return;
    auto it = instructions.find(uid);
    if (it == instructions.end()) return;
    const auto &r = rows[it->second];
    if (r.warp || !detailed(r.iteration) ||
        !custom_component::selected_cta(r.x, r.y, r.z)) return;
    auto &f = fronts[uid];
    if (f.stage) { ++errors; return; }
    ++f.collector_waits[!operands_ready ? 0 : blocker ? 1 : 2];
    if (operands_ready && blocker) {
      auto &b = f.collector_blocks[blocker];
      if (!b.count++) b.first = cycle;
      b.last = cycle;
    }
  }
  void front_stage(U uid, unsigned stage, U cycle) {
    if (!enabled() || !instructions.count(uid)) return;
    auto &f = fronts[uid];
    if (stage == 3 && f.stage == 3) return;
    if (stage != f.stage + 1 || stage > 3 ||
        (f.stage && cycle < f.time[f.stage - 1])) { ++errors; return; }
    f.time[stage - 1] = cycle; f.stage = stage;
  }
  void front_stall(U uid, bool icnt) {
    if (!enabled() || !instructions.count(uid)) return;
    auto it = fronts.find(uid);
    if (it == fronts.end() || it->second.stage != 3) { ++errors; return; }
    if (icnt) ++it->second.icnt_stalls;
    else ++it->second.coal_stalls;
  }
  void complete(U uid, U cycle, bool predicated_off) {
    if (!enabled()) return;
    auto it = instructions.find(uid); if (it == instructions.end()) return;
    auto &row = rows[it->second]; ++row.completed;
    if (predicated_off) ++inactive;
    row.time[LAST_COMPLETE] = std::max(row.time[LAST_COMPLETE],cycle);
    if (detailed(row.iteration)) std::printf("CUSTOM_ASYNC_COMPLETE %llu %llu\n",uid,cycle);
    auto front = fronts.find(uid);
    if (front != fronts.end()) {
      const auto &f = front->second;
      // Zero active lanes skip memory_cycle service but still complete at LSU.
      const unsigned last = predicated_off ? 2 : 3;
      if (f.stage != last || cycle < f.time[last - 1]) ++errors;
      if (detailed(row.iteration))
        std::printf("CUSTOM_ASYNC_FRONT %llu %llu %llu %llu %llu %llu\n",
            uid, f.time[0], f.time[1], f.time[2], f.icnt_stalls, f.coal_stalls);
      if (f.collector_waits[0] || f.collector_waits[1] || f.collector_waits[2]) {
        std::printf("CUSTOM_ASYNC_COLLECTOR %llu %llu %llu %llu\n", uid,
                    f.collector_waits[0], f.collector_waits[1], f.collector_waits[2]);
        for (const auto &item : f.collector_blocks)
          std::printf("CUSTOM_ASYNC_COLLECTOR_BLOCK %llu %llu %llu %llu %llu\n",
              uid, item.first, item.second.count, item.second.first, item.second.last);
      }
      fronts.erase(front);
    }
    instructions.erase(it);
  }
  void wait_release(U sid, U warp, U cycle) {
    if (!enabled()) return;
    auto it = current.find({sid,warp}); if (it == current.end()) return;
    auto &row = rows[it->second];
    if (row.time[WAIT_ENTER]) first(row.time[WAIT_RELEASE],cycle);
  }
  void barrier_release(U sid, U warp, U cycle) {
    if (!enabled()) return;
    auto it = current.find({sid,warp}); if (it == current.end()) return;
    auto &row = rows[it->second];
    if (row.time[BARRIER_ARRIVE]) first(row.time[BARRIER_RELEASE],cycle);
  }
  void request(U uid, U inst, int state, U cycle, U bytes = 32) {
    if (!enabled()) return;
    auto it = requests.find(uid);
    if (it == requests.end()) {
      auto instruction = instructions.find(inst);
      if (instruction == instructions.end()) return;
      it = requests.emplace(uid,Request{instruction->second,inst,-1,cycle}).first;
    }
    auto &request = it->second;
    if (request.state == state) return;  // Repeated status must preserve residence.
    auto &row = rows[request.row];
    if (cycle < request.begin) { ++errors; return; }
    if (request.state >= 0) {
      auto &span = residence[{row.launch,row.sid,request.state,state}];
      ++span.count; span.sum += cycle-request.begin;
      span.max = std::max(span.max,cycle-request.begin);
    }
    if (state == 7) {
      first(row.time[FIRST_INJECT],cycle); row.time[LAST_INJECT] = cycle;
      ++row.injected; row.bytes += bytes;
    }
    if (state == 23) {
      first(row.time[FIRST_RESPONSE],cycle); row.time[LAST_RESPONSE] = cycle;
      ++row.returned;
    }
    if (detailed(row.iteration))
      std::printf("CUSTOM_ASYNC_REQUEST %llu %llu %llu %llu %llu %llu %d %llu\n",
          uid,request.inst,row.launch,row.sid,row.warp,row.iteration,state,cycle);
    request.state = state; request.begin = cycle;
  }
  void finish_request(U uid, U cycle) {
    if (!enabled()) return;
    auto it = requests.find(uid); if (it == requests.end()) return;
    request(uid,it->second.inst,-2,cycle); requests.erase(uid);
  }
  void print() {
    if (!enabled() || rows.empty()) return;
    std::printf("CUSTOM_ASYNC_VERSION 1\n");
    for (const auto &r : rows) {
      std::printf("CUSTOM_ASYNC_ITER %llu %llu %llu %llu %llu %llu %llu %llu",
          r.launch,r.x,r.y,r.z,r.sid,r.warp,r.iteration,r.group);
      for (U time : r.time) std::printf(" %llu",time);
      std::printf(" %llu %llu %llu %llu %llu\n",r.issued,r.completed,r.injected,r.returned,r.bytes);
    }
    for (const auto &kv : residence)
      std::printf("CUSTOM_ASYNC_RESIDENCE %llu %llu %d %d %llu %llu %llu\n",
          std::get<0>(kv.first),std::get<1>(kv.first),std::get<2>(kv.first),
          std::get<3>(kv.first),kv.second.count,kv.second.sum,kv.second.max);
    std::printf("CUSTOM_ASYNC_INACTIVE %llu\n", inactive);
    std::printf("CUSTOM_ASYNC_PENDING %llu %llu %llu\n",(U)instructions.size(),(U)requests.size(),errors);
    rows.clear(); current.clear(); instructions.clear(); requests.clear();
    residence.clear(); fronts.clear(); errors = 0; inactive = 0;
  }
};
inline Observations &observations() { static Observations value; return value; }
}  // namespace custom_async_path
#endif
