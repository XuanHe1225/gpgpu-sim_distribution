#pragma once
#include "pnm_component_diagnostics.h"
#include <algorithm>
#include <cassert>
#include <limits>
#include <tuple>

namespace pnm_dependencies {
using U = unsigned long long;
struct Producer { U launch, pc, uid, issued; };
struct Result {
  U count = 0, sum = 0, min = std::numeric_limits<U>::max(), max = 0;
};
struct Observations {
  std::map<std::tuple<unsigned, unsigned, unsigned>, Producer> registers;
  std::map<U, unsigned> outstanding;
  std::map<std::tuple<U, U, U>, U> edges, last_cycle;
  std::map<std::pair<U, U>, Result> results;
  void reserve(unsigned sid, unsigned wid, unsigned reg, U launch, U pc, U uid, U cycle) {
    if (!pnm_components::enabled()) return;
    assert(registers.emplace(std::make_tuple(sid, wid, reg), Producer{launch, pc, uid, cycle}).second);
    ++outstanding[uid];
  }
  void blocked(unsigned sid, unsigned wid, unsigned reg, U consumer, U cycle) {
    if (!pnm_components::enabled()) return;
    auto it = registers.find(std::make_tuple(sid, wid, reg));
    if (it == registers.end()) return;
    auto key = std::make_tuple(it->second.launch, it->second.pc, consumer);
    auto last = last_cycle.find(key);
    if (last == last_cycle.end() || last->second != cycle) ++edges[key];
    last_cycle[key] = cycle;
  }
  void release(unsigned sid, unsigned wid, unsigned reg, U cycle) {
    if (!pnm_components::enabled()) return;
    auto it = registers.find(std::make_tuple(sid, wid, reg));
    if (it == registers.end()) return;
    auto p = it->second;
    assert(cycle >= p.issued && outstanding.at(p.uid));
    if (!--outstanding[p.uid]) {
      outstanding.erase(p.uid);
      auto &r = results[{p.launch, p.pc}];
      U elapsed = cycle - p.issued;
      ++r.count; r.sum += elapsed;
      r.min = std::min(r.min, elapsed); r.max = std::max(r.max, elapsed);
    }
    registers.erase(it);
  }
  void print() {
    if (!pnm_components::enabled()) return;
    std::printf("PNMS_DEP_VERSION 1\n");
    for (const auto &e : edges)
      std::printf("PNMS_DEP_EDGE %llu %llu %llu %llu\n", std::get<0>(e.first),
                  std::get<1>(e.first), std::get<2>(e.first), e.second);
    for (const auto &item : results) {
      const auto &r = item.second;
      std::printf("PNMS_DEP_RESULT %llu %llu %llu %llu %llu %llu\n",
                  item.first.first, item.first.second, r.count, r.sum, r.min, r.max);
    }
    std::printf("PNMS_DEP_PENDING %llu %llu\n", U(registers.size()), U(outstanding.size()));
    edges.clear(); last_cycle.clear(); results.clear();
  }
};
inline Observations &observations() { static Observations value; return value; }
}
