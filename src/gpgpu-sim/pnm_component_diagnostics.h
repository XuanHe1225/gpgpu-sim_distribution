// Read-only component observations. Disabled by default; no simulated work.
#ifndef PNM_COMPONENT_DIAGNOSTICS_H
#define PNM_COMPONENT_DIAGNOSTICS_H
#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>
#include <utility>

namespace pnm_components {
inline bool enabled() {
  static bool value = std::getenv("PNMSERVING_COMPONENT_DIAGNOSTICS") != nullptr;
  return value;
}
inline void clock(const char *name, unsigned launch, unsigned x, unsigned y,
                  unsigned z, unsigned warp, unsigned long long mask,
                  unsigned pc, unsigned long long cycle) {
  if (!enabled()) return;
  static const auto markers = [] {
    std::map<std::string, std::pair<unsigned, unsigned>> result;
    const char *path = std::getenv("PNMSERVING_COMPONENT_CLOCKS");
    if (!path) return result;
    FILE *input = std::fopen(path, "r");
    if (!input) { std::perror(path); std::abort(); }
    char kernel[1024]; unsigned start, stop;
    while (std::fscanf(input, "%1023s %x %x", kernel, &start, &stop) == 3) {
      if (start == stop || !result.emplace(kernel, std::make_pair(start, stop)).second)
        std::abort();
    }
    if (!std::feof(input)) std::abort();
    std::fclose(input);
    return result;
  }();
  auto it = markers.find(name);
  if (it == markers.end() || (pc != it->second.first && pc != it->second.second)) return;
  std::printf("PNMS_COMPONENT_CLOCK %s %u %u %u %u %u %llu %s %llu\n",
              name, launch, x, y, z, warp, mask,
              pc == it->second.first ? "start" : "stop", cycle);
}
struct Controller {
  // Bit masks: CCD=1, RCD/RCDWR=2, CCDL=4, turnaround=8, data queue=16.
  std::map<std::pair<unsigned, unsigned>, unsigned long long> blockers;
  unsigned long long rd_to_wr = 0, wr_to_rd = 0;
  unsigned long long run = 0, max_read_run = 0, max_write_run = 0;
  unsigned long long ready_but_unissued = 0;
  int direction = -1;
  void command(bool write) {
    if (!enabled()) return;
    if (direction != int(write)) {
      if (direction == 0) ++rd_to_wr;
      if (direction == 1) ++wr_to_rd;
      run = 0;
      direction = int(write);
    }
    ++run;
    auto &maximum = write ? max_write_run : max_read_run;
    if (run > maximum) maximum = run;
  }
  void blocked(unsigned all, unsigned common, bool ready) {
    if (!enabled()) return;
    ++blockers[{all, common}];
    if (ready) ++ready_but_unissued;
  }
  void print(unsigned id) const {
    if (!enabled()) return;
    std::printf("PNMS_COMPONENT_MC %u %llu %llu %llu %llu %llu\n", id,
                rd_to_wr, wr_to_rd, max_read_run, max_write_run, ready_but_unissued);
    for (const auto &p : blockers)
      std::printf("PNMS_COMPONENT_MASK %u %u %u %llu\n", id,
                  p.first.first, p.first.second, p.second);
  }
};
} // namespace pnm_components
#endif
