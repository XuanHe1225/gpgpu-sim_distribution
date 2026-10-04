#include "../src/gpgpu-sim/custom_insight_address.h"
#include <cassert>
#include <set>
#include <iostream>

int main() {
  using namespace custom_insight;
  constexpr U base = 140682702356480ull;
  std::set<U> original_sectors, local_sectors, local_lines, concentrated_lines;
  for (U lane=0; lane<32; ++lane) {
    const U a=base+(lane/8)*256+(lane%8)*16;
    const U local=map_weight(a,base,1), pch=map_weight(a,base,2);
    assert(unmap_weight(local,base,1)==a);
    assert(unmap_weight(pch,base,2)==a);
    auto before=decode(a), after=decode(local);
    assert(before.pch==after.pch && before.bank==after.bank && before.row==after.row);
    assert(decode(pch).pch<8 && (pch%128)==(a%128));
    original_sectors.insert(a/32); local_sectors.insert(local/32);
    local_lines.insert(local/128); concentrated_lines.insert(pch/128);
  }
  assert(original_sectors.size()==16);
  assert(local_sectors.size()==32 && local_lines.size()==32);
  assert(concentrated_lines.size()==4);
  // All 16-byte words in the 40 MiB weight allocation: inverse plus a bounded
  // image catches aliasing, high-address truncation and scalar-lane corruption.
  for (U a=base; a<base+41943040; a+=16) {
    for (unsigned mode: {0u,1u,2u}) {
      U b=map_weight(a,base,mode);
      assert(unmap_weight(b,base,mode)==a && b%16==0);
      assert(b>=base && b<base+(mode==2 ? 419430400ull : 41943040ull));
      for (U k: {0ull,1ull,7ull,15ull})
        assert(map_weight(a+k,base,mode)==b+k);
    }
  }
  Credits credits(32);
  for(unsigned uid=1;uid<=32;++uid) credits.inject(uid,0);
  assert(credits.full(0) && !credits.full(1));
  credits.reply(7); assert(!credits.full(0));
  credits.inject(33,0); assert(credits.full(0));
  credits.reply(999); assert(credits.full(0)); // downstream child, no credit
  for(unsigned uid=1;uid<=33;++uid) credits.reply(uid);
  assert(credits.pending()==0);
  std::cout << "address bijection, independent warp geometry and credits passed\n";
}
