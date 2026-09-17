// Optional replay of the static issue interval encoded in Volta/Ampere SASS.
// No latency fitting: sidecar values are checked against the captured binary.
#ifndef PNM_SASS_STALLS_H
#define PNM_SASS_STALLS_H
#include <cstdlib>
#include <fstream>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>

namespace pnm_sass {
struct IssueWindow {
  unsigned long long next = 0;
  bool ready(unsigned long long cycle) const { return cycle >= next; }
  void issued(unsigned long long cycle, unsigned stall) { next = cycle + stall; }
  void reset() { next = 0; }
};

class Controls {
 public:
  explicit Controls(std::istream &input) {
    std::string line;
    while (std::getline(input, line)) {
      std::istringstream row(line);
      std::string kernel, extra;
      unsigned pc, stall;
      if (!(row >> kernel >> std::hex >> pc >> std::dec >> stall) ||
          (row >> extra) || stall > 15 || pc % 16 ||
          !values.emplace(std::make_pair(kernel, pc), stall).second)
        throw std::runtime_error("invalid or duplicate SASS issue control");
    }
    if (!input.eof() || values.empty())
      throw std::runtime_error("missing SASS issue controls");
  }
  unsigned at(const std::string &kernel, unsigned pc) const {
    auto found = values.find(std::make_pair(kernel, pc));
    if (found == values.end())
      throw std::runtime_error("missing SASS issue control for " + kernel);
    return found->second;
  }
 private:
  std::map<std::pair<std::string, unsigned>, unsigned> values;
};

inline const Controls &controls() {
  static const Controls value = [] {
    const char *path = std::getenv("PNMSERVING_SASS_ISSUE_STALLS");
    if (!path) throw std::runtime_error("SASS issue controls are required");
    std::ifstream input(path);
    if (!input) throw std::runtime_error("cannot open SASS issue controls");
    return Controls(input);
  }();
  return value;
}
}  // namespace pnm_sass
#endif
