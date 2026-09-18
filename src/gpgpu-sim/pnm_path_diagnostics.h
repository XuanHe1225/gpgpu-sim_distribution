// Bounded, read-only path observations. No simulator state is changed here.
#ifndef PNM_PATH_DIAGNOSTICS_H
#define PNM_PATH_DIAGNOSTICS_H
#include "pnm_component_diagnostics.h"
#include "pnm_dependencies.h"
#include <algorithm>
#include <array>
#include <cassert>
#include <limits>
#include <tuple>
#include <vector>

namespace pnm_paths {
using U = unsigned long long;
constexpr U period = 64;
struct Span {
  U count = 0, sum = 0, min = std::numeric_limits<U>::max(), max = 0;
  void add(U v) { ++count; sum += v; min = std::min(min, v); max = std::max(max, v); }
};
struct Instruction { U launch, pc; std::array<U, 5> time{}; unsigned stage = 0; };
struct Pipe { U count = 0, max = 0; std::array<U, 4> sum{}; };
struct Request { unsigned partition, type; int state; U begin; };
struct Queue {
  U samples = 0, queued = 0, rowhit = 0, ready = 0, behind = 0, opposite = 0, age = 0;
};
struct Observations {
  std::map<U, Instruction> inflight;
  std::map<std::pair<U, U>, Pipe> pipes;
  std::map<U, std::pair<U, U>> previous;
  std::map<std::tuple<U, U, U>, Span> gaps;
  std::map<U, Request> requests;
  std::map<std::tuple<unsigned, unsigned, int, int>, Span> residency;
  std::map<unsigned, Queue> queues;
  std::map<unsigned, std::array<U, 32>> l2;
  std::map<std::tuple<U, U, U, U>, U> lsu;
  std::map<std::pair<unsigned, unsigned>, std::array<U, 8>> ctas;
  std::vector<std::array<U, 8>> completed_ctas;
  void issue(U uid, U launch, U pc, U cycle) {
    if (!pnm_components::enabled()) return;
    auto p = previous.find(launch);
    if (p != previous.end()) gaps[{launch, p->second.first, pc}].add(cycle - p->second.second);
    previous[launch] = {pc, cycle};
    Instruction inst{launch, pc}; inst.time[0] = cycle;
    assert(inflight.emplace(uid, inst).second);
  }
  void stage(U uid, unsigned stage, U cycle) {
    if (!pnm_components::enabled()) return;
    auto it = inflight.find(uid); if (it == inflight.end()) return;
    auto &v = it->second;
    assert(stage == v.stage + 1 && stage < 5 && cycle >= v.time[v.stage]);
    v.time[stage] = cycle; v.stage = stage;
    if (stage == 4) {
      auto &p = pipes[{v.launch, v.pc}]; ++p.count;
      for (unsigned i = 0; i < 4; ++i) p.sum[i] += v.time[i + 1] - v.time[i];
      p.max = std::max(p.max, cycle - v.time[0]); inflight.erase(it);
    }
  }
  void transition(U uid, unsigned partition, unsigned type, int old_state,
                  int new_state, U old_cycle, U cycle) {
    if (!pnm_components::enabled() || uid % period) return;
    auto it = requests.emplace(uid, Request{partition, type, old_state, old_cycle}).first;
    auto &r = it->second;
    if (r.state == new_state) return; // Repeated set_status must not lose residence time.
    assert(cycle >= r.begin);
    residency[{r.partition, r.type, r.state, new_state}].add(cycle - r.begin);
    r = {partition, type, new_state, cycle};
  }
  void finish_request(U uid, U cycle) {
    if (!pnm_components::enabled() || uid % period) return;
    auto it = requests.find(uid); if (it == requests.end()) return;
    auto &r = it->second; assert(cycle >= r.begin);
    residency[{r.partition, r.type, r.state, -1}].add(cycle - r.begin);
    requests.erase(it);
  }
  void cta_start(U launch, unsigned sid, unsigned slot, unsigned x, unsigned y,
                 unsigned z, U cycle) {
    if (!pnm_components::enabled()) return;
    assert(ctas.emplace(std::make_pair(sid, slot),
                       std::array<U, 8>{launch, sid, slot, x, y, z, cycle, 0}).second);
  }
  void cta_end(unsigned sid, unsigned slot, U cycle) {
    if (!pnm_components::enabled()) return;
    auto it = ctas.find({sid, slot}); assert(it != ctas.end());
    it->second[7] = cycle; completed_ctas.push_back(it->second); ctas.erase(it);
  }
  void print() {
    if (!pnm_components::enabled()) return;
    pnm_dependencies::observations().print();
    std::printf("PNMS_PATH_VERSION 1 %llu\n", period);
    for (const auto &kv : pipes) {
      const auto &p = kv.second;
      std::printf("PNMS_PATH_PIPE %llu %llu %llu %llu %llu %llu %llu %llu\n",
          kv.first.first, kv.first.second, p.count, p.sum[0], p.sum[1], p.sum[2], p.sum[3], p.max);
    }
    for (const auto &kv : gaps) {
      const auto &s = kv.second;
      std::printf("PNMS_PATH_GAP %llu %llu %llu %llu %llu %llu %llu\n",
          std::get<0>(kv.first), std::get<1>(kv.first), std::get<2>(kv.first), s.count, s.sum, s.min, s.max);
    }
    for (const auto &kv : residency) {
      const auto &s = kv.second;
      std::printf("PNMS_PATH_RESIDENCE %u %u %d %d %llu %llu %llu\n",
          std::get<0>(kv.first), std::get<1>(kv.first), std::get<2>(kv.first), std::get<3>(kv.first), s.count, s.sum, s.max);
    }
    for (const auto &kv : queues) {
      const auto &q = kv.second;
      std::printf("PNMS_PATH_QUEUE %u %llu %llu %llu %llu %llu %llu %llu\n",
          kv.first, q.samples, q.queued, q.rowhit, q.ready, q.behind, q.opposite, q.age);
    }
    for (const auto &kv : l2)
      for (unsigned mask = 0; mask < 32; ++mask)
        if (kv.second[mask]) std::printf("PNMS_PATH_L2 %u %u %llu\n", kv.first, mask, kv.second[mask]);
    for (const auto &kv : lsu)
      std::printf("PNMS_PATH_LSU %llu %llu %llu %llu %llu\n", std::get<0>(kv.first),
          std::get<1>(kv.first), std::get<2>(kv.first), std::get<3>(kv.first), kv.second);
    for (const auto &c : completed_ctas)
      std::printf("PNMS_PATH_CTA %llu %llu %llu %llu %llu %llu %llu %llu\n",
          c[0], c[1], c[2], c[3], c[4], c[5], c[6], c[7]);
    std::printf("PNMS_PATH_PENDING %llu %llu %llu\n", (U)inflight.size(), (U)requests.size(), (U)ctas.size());
    pipes.clear(); gaps.clear(); previous.clear(); residency.clear(); queues.clear();
    l2.clear(); lsu.clear(); completed_ctas.clear();
  }
};
inline Observations &observations() { static Observations value; return value; }
} // namespace pnm_paths
#endif
