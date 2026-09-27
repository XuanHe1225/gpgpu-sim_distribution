#pragma once
#include <cstdio>
#include <cstdlib>
#include <string>

// ADR-0103: deterministic sensitivity control, not a claimed NVIDIA policy.
namespace custom_warp_order {
inline void validate(unsigned mode, unsigned slots, unsigned schedulers,
                     const std::string &policy) {
  if (mode > 2 || (mode && (slots != 64 || schedulers != 4 || policy != "lrr"))) {
    std::fprintf(stderr, "custom_warp_swizzle requires mode 0/1/2; enabled modes require 64 warps, 4 schedulers and lrr\n");
    std::abort();
  }
}
inline unsigned slot(unsigned mode, unsigned sm, unsigned slots, unsigned rank) {
  return mode ? ((2 * sm + 1) * rank + sm) % slots : rank;
}
inline unsigned buffer_initial(unsigned mode, unsigned sm, unsigned slots) {
  return mode == 2 ? sm % slots : 0;
}
inline unsigned buffer_candidate(unsigned mode, unsigned sm, unsigned slots,
                                 unsigned last, unsigned rank) {
  return (last + (rank + 1) * (mode == 2 ? 2 * sm + 1 : 1)) % slots;
}
}  // namespace custom_warp_order
