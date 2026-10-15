#pragma once
#include <array>
#include <cerrno>
#include <climits>
#include <cstdio>
#include <cstdlib>

namespace custom_component {
// Select one CTA for existing read-only observations; never filter execution.
inline bool selected_cta(unsigned x, unsigned y, unsigned z) {
  static const auto target = [] {
    std::array<unsigned, 3> result{{0, 0, 0}};
    const char *cursor = std::getenv("CUSTOM_COMPONENT_CTA");
    if (!cursor) return result;
    for (unsigned i = 0; i < 3; ++i) {
      char *end = nullptr;
      errno = 0;
      const unsigned long value = std::strtoul(cursor, &end, 10);
      if (*cursor < '0' || *cursor > '9' || errno || value > UINT_MAX ||
          (i < 2 ? *end != ',' : *end != '\0')) {
        std::fprintf(stderr, "CUSTOM_COMPONENT_CTA requires unsigned x,y,z\n");
        std::abort();
      }
      result[i] = value;
      if (i < 2) cursor = end + 1;
    }
    std::printf("CUSTOM_COMPONENT_CTA %u %u %u\n", result[0], result[1], result[2]);
    return result;
  }();
  return target[0] == x && target[1] == y && target[2] == z;
}
} // namespace custom_component
