#pragma once
#include <algorithm>
#include <cassert>
#include <functional>
#include <queue>
#include <vector>

// Diagnostic matching only: never supplies a packet to the real arbiter.
namespace custom_icnt_queue {
using U = unsigned long long;
inline bool &enabled_flag() { static bool value=false; return value; }
inline bool sample_tick(U tick) { return tick%64 == ((tick/64)%64*17)%64; }

inline unsigned maximum_matching(const std::vector<std::vector<unsigned>> &edges,
                                 const std::vector<bool> &ready) {
  const unsigned n=edges.size();
  std::vector<int> input(n,-1),output(ready.size(),-1),distance(n);
  unsigned cardinality=0;
  for(;;) {
    std::queue<unsigned> bfs;
    std::fill(distance.begin(),distance.end(),-1);
    for(unsigned i=0;i<n;++i) if(input[i]<0) {distance[i]=0;bfs.push(i);}
    bool free_output=false;
    while(!bfs.empty()) {
      const unsigned i=bfs.front();bfs.pop();
      for(auto d:edges[i]) if(ready[d]) {
        const int other=output[d];
        if(other<0) free_output=true;
        else if(distance[other]<0) {distance[other]=distance[i]+1;bfs.push(other);}
      }
    }
    if(!free_output) break;
    std::function<bool(unsigned)> augment=[&](unsigned i) {
      for(auto d:edges[i]) if(ready[d]) {
        const int other=output[d];
        if(other<0 || (distance[other]==distance[i]+1 && augment(other))) {
          input[i]=d;output[d]=i;return true;
        }
      }
      distance[i]=-1;return false;
    };
    unsigned gained=0;
    for(unsigned i=0;i<n;++i) if(input[i]<0 && augment(i)) ++gained;
    assert(gained);cardinality+=gained;
  }
  return cardinality;
}

struct Sample {
  U tick=0;
  unsigned ports=0,active_bound=0,all_matching=0,ready_matching=0;
  unsigned head_matching=0,grants=0,queued=0;
};

class Census {
 public:
  Census(unsigned inputs,unsigned outputs):counts(inputs,std::vector<unsigned>(outputs,0)),sizes(inputs,0),n_outputs(outputs) {}
  void push(unsigned input,unsigned output) {
    assert(input<counts.size() && output<n_outputs);++counts[input][output];++sizes[input];
  }
  void pop(unsigned input,unsigned output) {
    assert(input<counts.size() && output<n_outputs && counts[input][output] && sizes[input]);
    --counts[input][output];--sizes[input];
  }
  const std::vector<std::vector<unsigned>> &population() const {return counts;}
  unsigned size(unsigned input) const {return sizes.at(input);}
  Sample snapshot(const std::vector<int> &heads,const std::vector<bool> &ready) const {
    assert(heads.size()==counts.size() && ready.size()==n_outputs);
    Sample s;s.ports=std::min(unsigned(counts.size()),n_outputs);
    std::vector<std::vector<unsigned>> edges(counts.size());
    std::vector<bool> all_ready(n_outputs,true),head_destinations(n_outputs,false);
    unsigned active=0;
    for(unsigned i=0;i<counts.size();++i) {
      assert((heads[i]>=0)==(sizes[i]>0));
      s.queued+=sizes[i];active+=sizes[i]>0;
      if(heads[i]>=0) {
        assert(unsigned(heads[i])<n_outputs && counts[i][heads[i]]);
        if(ready[heads[i]]) head_destinations[heads[i]]=true;
      }
      for(unsigned d=0;d<n_outputs;++d) if(counts[i][d]) edges[i].push_back(d);
    }
    s.active_bound=std::min(active,n_outputs);
    s.all_matching=maximum_matching(edges,all_ready);
    s.ready_matching=(ready==all_ready)?s.all_matching:maximum_matching(edges,ready);
    s.head_matching=std::count(head_destinations.begin(),head_destinations.end(),true);
    assert(s.head_matching<=s.ready_matching && s.ready_matching<=s.all_matching && s.all_matching<=s.active_bound);
    return s;
  }
 private:
  std::vector<std::vector<unsigned>> counts;
  std::vector<unsigned> sizes;
  unsigned n_outputs;
};
}
