// Optional observation only: no queue, address, configuration or timing changes.
#ifndef PNM_MEMORY_EVENTS_H
#define PNM_MEMORY_EVENTS_H
#include <cstdio>
#include <cstdlib>
#include "mem_fetch.h"

namespace pnm_events {
unsigned long long cycle(mem_fetch *mf);
inline FILE *sink() {
  static FILE *file = []() -> FILE * {
    const char *path = std::getenv("PNMSERVING_MEMORY_EVENTS");
    if (!path || !*path) return nullptr;
    FILE *out = std::fopen(path, "w");
    if (!out) { std::perror(path); std::abort(); }
    std::setvbuf(out, nullptr, _IOFBF, 1 << 20);
    std::fprintf(out, "seq,cycle,dram_cycle,event,request,parent,related,inst,sm,warp,dynamic_warp,cta_x,cta_y,cta_z,pc,address,bytes,mask,channel,bank,row,col,detail,aux\n");
    return out;
  }();
  return file;
}
inline void emit(const char *event, unsigned long long cycle,
                 const warp_inst_t *inst, unsigned sm,
                 unsigned request = 0, unsigned parent = 0, unsigned related = 0,
                 unsigned long long address = 0, unsigned bytes = 0,
                 unsigned long long mask = 0, const addrdec_t *da = nullptr,
                 const char *detail = "", unsigned long long aux = 0,
                 unsigned long long dram_cycle = 0) {
  FILE *out = sink();
  if (!out) return;
  static unsigned long long seq = 0;
  bool valid = inst && !inst->empty();
  dim3 cta = valid ? inst->get_cuda_cta_id() : dim3(-1, -1, -1);
  std::fprintf(out,
      "%llu,%llu,%llu,%s,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%llu,%llu,%u,%llu,%u,%u,%llu,%llu,%s,%llu\n",
      ++seq, cycle, dram_cycle, event, request, parent, related,
      valid ? inst->get_uid() : 0, sm, valid ? inst->warp_id() : ~0u,
      valid ? inst->dynamic_warp_id() : ~0u, cta.x, cta.y, cta.z,
      valid ? (unsigned long long)inst->pc : 0, address, bytes, mask,
      da ? da->chip : ~0u, da ? da->bk : ~0u,
      da ? (unsigned long long)da->row : 0,
      da ? (unsigned long long)da->col : 0, detail, aux);
}
inline void request(const char *event, unsigned long long cycle, mem_fetch *mf,
                    const char *detail = "", unsigned related = 0,
                    unsigned long long aux = 0,
                    unsigned long long dram_cycle = 0) {
  if (!sink() || !mf) return;
  emit(event, cycle, &mf->get_inst(), mf->get_sid(), mf->get_request_uid(),
       mf->get_original_mf() ? mf->get_original_mf()->get_request_uid() : 0,
       related, mf->get_addr(), mf->get_data_size(),
       mf->get_access_warp_mask().to_ullong(), &mf->get_tlx_addr(), detail,
       aux, dram_cycle);
}
inline void instruction(const char *event, unsigned long long cycle,
                        const warp_inst_t &inst, unsigned sm, bool lanes) {
  if (!sink() || !(inst.is_load() || inst.is_store())) return;
  emit(event, cycle, &inst, sm, 0, 0, 0, 0, inst.data_size,
       inst.get_active_mask().to_ullong(), nullptr,
       inst.m_is_ldgsts ? "LDGSTS" : inst.is_load() ? "load" : "store",
       inst.accessq_count());
  // The approved SM80 experiment uses scalar/vector global accesses only.
  if (lanes && inst.space.get_type() == global_space) {
    for (unsigned lane = 0; lane < inst.warp_size(); ++lane)
      if (inst.active(lane))
        emit("lane", cycle, &inst, sm, 0, 0, 0, inst.get_addr(lane),
             inst.data_size, 1ull << lane, nullptr, "global", lane);
  }
}
}  // namespace pnm_events
#endif
