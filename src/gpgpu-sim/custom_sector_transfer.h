// Scheduling metadata only: never combine, replace or complete mem_fetches.
#ifndef CUSTOM_SECTOR_TRANSFER_H
#define CUSTOM_SECTOR_TRANSFER_H
#include <cstdio>

namespace custom_sector_transfer {
using U = unsigned long long;
struct Sector {
  bool eligible = false;
  // Separate from bypass-sector packing eligibility. Unknown operations form
  // an ordering barrier for the optional request lookahead experiment.
  bool request_known = false, ordinary_read = false;
  U address = 0, instruction = 0;
  unsigned shader = 0, warp = 0;
  unsigned bit() const { return 1u << ((address >> 5) & 3); }
  bool valid() const { return eligible && !(address & 31); }
  bool matches(const Sector &head, unsigned used) const {
    return valid() && head.valid() && instruction == head.instruction &&
           shader == head.shader && warp == head.warp &&
           (address >> 7) == (head.address >> 7) && !(used & bit());
  }
};
struct Counters {
  U groups = 0, sectors = 0, payload_bytes = 0;
  void add(unsigned count) {
    if (!count) return;
    ++groups; sectors += count; payload_bytes += 32ull * count;
  }
  void print(const char *stage) const {
    std::printf("custom_sector_transfer stage=%s groups=%llu sectors=%llu payload_bytes=%llu\n",
                stage, groups, sectors, payload_bytes);
  }
};
inline Counters &icnt_receive() { static Counters c; return c; }
inline Counters &lsu_receive() { static Counters c; return c; }
inline void print() {
  icnt_receive().print("icnt_to_cluster");
  lsu_receive().print("cluster_to_lsu");
}
}  // namespace custom_sector_transfer
#endif
