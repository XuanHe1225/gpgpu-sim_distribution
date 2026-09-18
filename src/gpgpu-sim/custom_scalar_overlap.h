#pragma once
#include <cassert>
#include <map>
#include <set>
#include <vector>

// No timing prediction and no storage capacity: describes values held in
// existing collector/FU input slots. The core gates actual FU entry.
namespace custom_overlap {
using U = unsigned long long;
struct Descriptor {
  unsigned warp=0, unit=0;
  U mask=0;
  std::set<unsigned> sources, outputs;
};
struct Node {
  Descriptor desc;
  std::map<unsigned,U> bindings;
  std::set<U> waiting;
  bool entered=false;
};
struct Tracker {
  std::map<U,Node> nodes;
  std::map<std::pair<unsigned,unsigned>,U> owners;
  std::map<unsigned,U> prepared;
  bool blocked(unsigned warp) const { return prepared.count(warp); }
  bool eligible(const Descriptor &d,const std::set<unsigned> &pending) const {
    if(!d.unit || !d.mask || pending.empty() || blocked(d.warp)) return false;
    for(unsigned r:pending) {
      if(!d.sources.count(r)) return false; // unsupported pure WAW
      auto owner=owners.find({d.warp,r}); if(owner==owners.end()) return false;
      auto p=nodes.find(owner->second);
      if(p==nodes.end() || !p->second.entered ||
         p->second.desc.unit!=d.unit || p->second.desc.mask!=d.mask) return false;
    }
    return true;
  }
  std::map<unsigned,U> bind(const Descriptor &d,const std::set<unsigned> &pending) const {
    assert(eligible(d,pending));
    std::map<unsigned,U> result;
    for(unsigned r:pending) result[r]=owners.at({d.warp,r});
    return result;
  }
  void issue(U uid,const Descriptor &d,const std::map<unsigned,U> &bindings) {
    assert(d.unit && !blocked(d.warp));
    Node n; n.desc=d;n.bindings=bindings;
    for(const auto &b:bindings) {assert(nodes.count(b.second));n.waiting.insert(b.second);}
    assert(nodes.emplace(uid,n).second);
    if(!bindings.empty()) assert(prepared.emplace(d.warp,uid).second);
    for(unsigned r:d.outputs) owners[{d.warp,r}]=uid;
  }
  bool ready(U uid) const {
    auto it=nodes.find(uid);return it==nodes.end() || it->second.waiting.empty();
  }
  bool bypasses(U uid,unsigned reg) const {
    auto it=nodes.find(uid);return it!=nodes.end() && it->second.bindings.count(reg);
  }
  void enter(U uid) {
    auto it=nodes.find(uid);if(it==nodes.end()) return;
    assert(ready(uid) && !it->second.entered); it->second.entered=true;
    auto p=prepared.find(it->second.desc.warp);
    if(p!=prepared.end() && p->second==uid) prepared.erase(p);
  }
  void arrive(U uid) {
    auto it=nodes.find(uid);if(it==nodes.end()) return;
    assert(it->second.entered);
    for(auto &n:nodes) n.second.waiting.erase(uid);
    for(unsigned r:it->second.desc.outputs) {
      auto p=owners.find({it->second.desc.warp,r});
      if(p!=owners.end() && p->second==uid) owners.erase(p);
    }
    nodes.erase(it);
  }
  bool empty() const {return nodes.empty() && owners.empty() && prepared.empty();}
};
}
