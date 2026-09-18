#pragma once
#include <sstream>
#include <stdexcept>
#include <string>

namespace pnm_carveout {
inline bool valid(unsigned bytes) {
  for (unsigned kb : {0, 8, 16, 32, 64, 100, 132, 164})
    if (bytes == kb * 1024) return true;
  return false;
}
inline unsigned associativity(unsigned shared, unsigned unified, unsigned max_assoc) {
  if (!valid(shared) || shared >= unified || !max_assoc ||
      (static_cast<unsigned long long>(unified - shared) * max_assoc) % unified)
    throw std::runtime_error("unrepresentable measured carveout geometry");
  return static_cast<unsigned long long>(unified - shared) * max_assoc / unified;
}
struct Launch { unsigned shared, allocated; };
inline Launch parse(std::istream &input, const std::string &expected, unsigned smem) {
  unsigned version;
  std::string name, extra;
  Launch row;
  if (!(input >> version >> name >> row.shared >> row.allocated) ||
      version != 1 || name != expected || !valid(row.shared) ||
      row.allocated < smem || row.allocated > row.shared || (input >> extra))
    throw std::runtime_error("missing or invalid measured launch carveout");
  return row;
}
}
