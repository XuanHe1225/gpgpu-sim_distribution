#pragma once
// Experiment-only address bijections and a finite read-injection credit gate.
// No change is active unless CUSTOM_INSIGHT_* is explicitly configured.
#include <array>
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <unordered_map>

namespace custom_insight {
using U = unsigned long long;
constexpr U weight_size=41943040, row_stride=2621440;
struct Coordinates { U pch,bank,row,col,offset; };
inline Coordinates decode(U a) {
  const U stripe=a>>8, local=((stripe/80)<<8)|(a&255);
  // Keep the high row bits here: truncating them would alias native trace VAs.
  return {stripe%80, (((local>>11)&15)<<1)|((local>>7)&1),
          local>>15, (((local>>8)&7)<<2)|((local>>5)&3), local&31};
}
inline U encode(Coordinates d) {
  const U local=(d.row<<15)|((d.bank>>1)<<11)|((d.col>>2)<<8)|
                ((d.bank&1)<<7)|((d.col&3)<<5)|d.offset;
  return (((local>>8)*80+d.pch)<<8)|(local&255);
}
inline U map_weight(U a,U base,unsigned mode) {
  assert(mode<=3 && base%row_stride==0 && a>=base && a<base+weight_size);
  auto d=decode(a-base);
  if(mode&1) {
    const U v=(d.col<<1)|(d.offset>>4), swapped=((v&7)<<3)|(v>>3);
    d.col=swapped>>1; d.offset=(d.offset&15)|((swapped&1)<<4);
  }
  if(mode&2) { d.row=d.row*10+d.pch/8; d.pch%=8; }
  return base+encode(d);
}
inline U unmap_weight(U a,U base,unsigned mode) {
  assert(mode<=3 && a>=base);
  auto d=decode(a-base);
  if(mode&2) { d.pch+=8*(d.row%10); d.row/=10; }
  if(mode&1) {
    const U v=(d.col<<1)|(d.offset>>4), swapped=((v&7)<<3)|(v>>3);
    d.col=swapped>>1; d.offset=(d.offset&15)|((swapped&1)<<4);
  }
  return base+encode(d);
}
inline U env_number(const char *name,U fallback=0) {
  const char *s=std::getenv(name); if(!s || !*s) return fallback;
  char *end=nullptr; U value=std::strtoull(s,&end,0);
  if(*end || *s=='-') { std::fprintf(stderr,"invalid %s\n",name); std::abort(); }
  return value;
}
struct Settings {
  unsigned mode=env_number("CUSTOM_INSIGHT_ADDRESS_MODE");
  unsigned cap=env_number("CUSTOM_INSIGHT_READ_CAP");
  U base=env_number("CUSTOM_INSIGHT_WEIGHT_BASE");
  U activation=env_number("CUSTOM_INSIGHT_ACTIVATION_BASE");
  U activation_size=env_number("CUSTOM_INSIGHT_ACTIVATION_BYTES",8388608);
  const char *report=std::getenv("CUSTOM_INSIGHT_REPORT");
  Settings() {
    if(mode>3 || cap>4096 || ((mode || report) && (!base || base%row_stride))) {
      std::fprintf(stderr,"invalid insight experiment settings\n"); std::abort();
    }
  }
};
inline const Settings &settings() { static Settings s; return s; }
inline U transform(U a) {
  const auto &s=settings();
  return s.mode && a>=s.base && a<s.base+weight_size ? map_weight(a,s.base,s.mode) : a;
}
inline unsigned operand(U a) {
  const auto &s=settings();
  if(a>=s.activation && a<s.activation+s.activation_size) return 0;
  const U span=(s.mode&2)?weight_size*10:weight_size;
  if(a>=s.base && a<s.base+span && (!(s.mode&2) || decode(a-s.base).pch<8)) return 1;
  return 2;
}
struct Credits {
  unsigned limit;
  std::array<unsigned,108> counts{}, peaks{};
  std::unordered_map<U,unsigned> owners;
  explicit Credits(unsigned n):limit(n){}
  bool full(unsigned sm) const { assert(sm<108); return limit && counts[sm]>=limit; }
  void inject(U uid,unsigned sm) {
    assert(sm<108 && !owners.count(uid)); owners.emplace(uid,sm);
    ++counts[sm]; if(counts[sm]>peaks[sm]) peaks[sm]=counts[sm];
    assert(!limit || counts[sm]<=limit);
  }
  void reply(U uid) {
    auto it=owners.find(uid); if(it==owners.end()) return;
    assert(counts[it->second]); --counts[it->second]; owners.erase(it);
  }
  size_t pending() const { return owners.size(); }
};
inline Credits &credits() { static Credits c(settings().cap); return c; }
inline bool enabled() { return settings().report && *settings().report; }
inline bool cap_full(unsigned sm) { return settings().cap && credits().full(sm); }
}
