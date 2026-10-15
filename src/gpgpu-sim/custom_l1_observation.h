#pragma once
#include <cstdio>
#include <cstdlib>
#include <initializer_list>

// Read-only observations of one SM in [begin,end), using core timestamps.
namespace custom_l1_queue {
using U = unsigned long long;
struct Window { bool enabled=false; unsigned sm=0; U begin=0,end=0; };
inline bool active(unsigned sm,U cycle) {
  static const Window window=[] {
    Window w;
    const char *text=std::getenv("CUSTOM_L1_QUEUE_OBSERVATION");
    if(!text) return w;
    char extra;
    if(std::sscanf(text,"%u %llu %llu %c",&w.sm,&w.begin,&w.end,&extra)!=3 ||
       w.sm>=108 || w.begin>=w.end || w.end>0x7fffffffffffffffull) {
      std::fprintf(stderr,"Invalid CUSTOM_L1_QUEUE_OBSERVATION: expected SM BEGIN END\n");
      std::exit(2);
    }
    w.enabled=true; return w;
  }();
  return window.enabled && sm==window.sm && cycle>=window.begin && cycle<window.end;
}
inline void record(const char *kind,unsigned sm,U cycle,std::initializer_list<U> fields) {
  if(!active(sm,cycle)) return;
  std::printf("CUSTOM_L1_QUEUE %s %llu %u",kind,cycle,sm);
  for(U value:fields) std::printf(" %llu",value);
  std::printf("\n");
}
} // namespace custom_l1_queue
