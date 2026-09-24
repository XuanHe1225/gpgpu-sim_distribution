// CPU regression against the production decoder, cache indexer and DRAM cycle.
// No CUDA kernel, GPU allocation, synthetic performance claim or copied timers.
#include "gpgpu-sim/gpu-sim.h"
#include "gpgpu-sim/dram.h"
#include "gpgpu-sim/dram_sched.h"
#include "gpgpu-sim/mem_fetch.h"
#include "gpgpu-sim/mem_latency_stat.h"
#include "gpgpu-sim/icnt_wrapper.h"
#include "gpgpu_context.h"
#include <cassert>
#include <cstdint>
#include <random>
#include <vector>

class custom_cpu_gpu : public gpgpu_sim {
 public:
  custom_cpu_gpu(const gpgpu_sim_config &cfg, gpgpu_context *ctx)
      : gpgpu_sim(cfg, ctx) {}
  void createSIMTCluster() override {}
};

// Independent mixed-radix reference, deliberately not the decoder bit masks.
static uint64_t address(unsigned pch, unsigned bank, unsigned row,
                        unsigned col = 0, unsigned offset = 0) {
  return 2621440ull * row + 1310720ull * (bank / 16) +
         163840ull * ((bank % 16) / 2) + 20480ull * (col / 4) +
         256ull * pch + 128ull * (bank % 2) + 32ull * (col % 4) + offset;
}

static gpgpu_sim_config *configuration(gpgpu_context *ctx, const char *base,
                                      const char *profile, const char *extra = nullptr) {
  auto *cfg = new gpgpu_sim_config(ctx);
  auto parser = option_parser_create();
  ctx->ptx_reg_options(parser);
  ctx->func_sim->ptx_opcocde_latency_options(parser);
  icnt_reg_options(parser);
  cfg->reg_options(parser);
  option_parser_cfgfile(parser, base);
  option_parser_cfgfile(parser, profile);
  if (extra) option_parser_cfgfile(parser, extra);
  cfg->init();
  return cfg;
}

class custom_dram_sid_test {
 public:
  static memory_config &memory(gpgpu_sim_config *cfg) { return cfg->m_memory_config; }
  static void grouping(memory_config &mc, bool sid) {
    assert(mc.custom_dram_sid_bank_groups == sid);
    dram_t d(0, &mc, nullptr, nullptr, nullptr);
    for (unsigned b = 0; b < mc.nbk; ++b) {
      unsigned expected = sid ? 4 * (b / 16) + (b % 4) : b % mc.nbkgrp;
      assert(d.get_bankgrp_number(b) == expected);
      if (sid) assert(d.bk[b]->bkgrpindex == expected);
    }
  }

  static void queued_banks(gpgpu_sim_config *cfg, gpgpu_sim *gpu) {
    auto &mc = cfg->m_memory_config;
    memory_stats_t stats(cfg->num_shader(), &cfg->m_shader_config, &mc, gpu);
    dram_t d(0, &mc, &stats, nullptr, gpu);
    gpu->gpu_sim_cycle = 0;
    for (unsigned b = 0; b < 32; ++b)
      d.m_frfcfs_scheduler->add_req(request(mc, gpu, b, b + 1));
    for (unsigned t = 0; t < 512; ++t) {
      gpu->gpu_sim_cycle = t;
      d.cycle();
    }
    assert(d.n_rd == 32 && d.n_act == 32 && d.n_pre == 0);
    assert(d.m_frfcfs_scheduler->num_pending() == 0);
    for (unsigned b = 0; b < 32; ++b)
      assert(d.bk[b]->curr_row == b + 1 && d.bk[b]->n_access == 1);
    puts("CUSTOM_SID_QUEUE banks=32 RD=32 ACT=32 PRE=0");
  }

  static dram_req_t *request(memory_config &mc, gpgpu_sim *gpu,
                             unsigned bank, unsigned row, bool write = false) {
    mem_access_t access(write ? GLOBAL_ACC_W : GLOBAL_ACC_R,
                        address(0, bank, row), 32, write, mc.gpgpu_ctx);
    auto *mf = new mem_fetch(access, static_cast<const warp_inst_t *>(nullptr),
                             0, 8, 0, 0, 0, &mc, gpu->gpu_sim_cycle);
    auto *req = new dram_req_t(mf, mc.nbk, mc.dram_bnk_indexing_policy, gpu);
    assert(req->bk == bank && req->row == row);
    return req;
  }

  static void column_pair(memory_config &mc, gpgpu_sim *gpu,
                          unsigned other, bool write, unsigned interval) {
    dram_t d(0, &mc, nullptr, nullptr, gpu);
    for (unsigned b : {0u, other}) {
      d.bk[b]->state = BANK_ACTIVE;
      d.bk[b]->curr_row = b == 0 ? 2 : 7;
      d.bk[b]->mrq = request(mc, gpu, b, d.bk[b]->curr_row, write);
    }
    std::vector<unsigned> issued;
    for (unsigned t = 0; t < 10; ++t) {
      gpu->gpu_sim_cycle = t;
      auto before = d.n_rd + d.n_wr;
      d.cycle();
      if (d.n_rd + d.n_wr != before) issued.push_back(t);
    }
    assert(issued.size() == 2 && issued[0] == 0 && issued[1] == interval);
    assert(d.n_act == 0 && d.n_pre == 0);
    assert(d.bk[0]->curr_row == 2 && d.bk[other]->curr_row == 7);
    printf("CUSTOM_SID_PAIR %s 0 %u %u\n", write ? "WR" : "RD", other, interval);
  }

  static void row_conflict(memory_config &mc, gpgpu_sim *gpu) {
    dram_t d(0, &mc, nullptr, nullptr, gpu);
    d.bk[0]->mrq = request(mc, gpu, 0, 1);
    bool second = false;
    std::vector<unsigned> acts, pres, reads;
    for (unsigned t = 0; t < 110; ++t) {
      gpu->gpu_sim_cycle = t;
      auto a = d.n_act, p = d.n_pre, r = d.n_rd;
      d.cycle();
      if (a != d.n_act) acts.push_back(t);
      if (p != d.n_pre) pres.push_back(t);
      if (r != d.n_rd) reads.push_back(t);
      if (!second && d.n_rd == 1) {
        d.bk[0]->mrq = request(mc, gpu, 0, 2);
        second = true;
      }
    }
    assert((acts == std::vector<unsigned>{0, 72}));
    assert((pres == std::vector<unsigned>{50}));
    assert((reads == std::vector<unsigned>{22, 94}));
    printf("CUSTOM_SID_ROW ACT=0,72 PRE=50 RD=22,94\n");
  }
};

int main(int argc, const char **argv) {
  assert(argc == 4 || argc == 5);
  auto *ctx = new gpgpu_context();
  auto *cfg = configuration(ctx, argv[1], argv[2], argc == 5 ? argv[4] : nullptr);
  auto *old = configuration(ctx, argv[1], argv[3]);
  auto &mc = custom_dram_sid_test::memory(cfg);
  auto &legacy = custom_dram_sid_test::memory(old);
  assert(mc.nbk == 32 && mc.nbkgrp == 8 && mc.dram_atom_size == 32);
  custom_dram_sid_test::grouping(mc, true);
  custom_dram_sid_test::grouping(legacy, false);
  uint64_t checked = 0;
  auto check = [&](uint64_t pa) {
    addrdec_t actual, previous;
    mc.m_address_mapping.addrdec_tlx(pa, &actual, 0);
    legacy.m_address_mapping.addrdec_tlx(pa, &previous, 0);
    uint64_t rest = pa % (20ull << 30);
    auto offset = rest % 32; rest /= 32;
    auto cl = rest % 4; rest /= 4;
    auto bl = rest % 2; rest /= 2;
    auto pch = rest % 80; rest /= 80;
    auto ch = rest % 8; rest /= 8;
    auto bh = rest % 8; rest /= 8;
    auto sid = rest % 2; rest /= 2;
    assert(actual.chip == pch && actual.bk == 16 * sid + 2 * bh + bl);
    assert(actual.row == rest && actual.col == 32 * (4 * ch + cl) + offset);
    assert(actual.burst == offset && actual.sub_partition == 2 * pch + bl);
    assert(actual.sub_partition == previous.sub_partition);
    assert(mc.m_address_mapping.partition_address(pa) == legacy.m_address_mapping.partition_address(pa));
    assert(mc.m_L2_config.set_index(pa) == legacy.m_L2_config.set_index(pa));
    assert(mc.m_L2_config.tag(pa) == legacy.m_L2_config.tag(pa));
    ++checked;
  };
  for (unsigned row : {0u, 1u, 8191u})
    for (unsigned bank = 0; bank < 32; ++bank)
      for (unsigned pch = 0; pch < 80; ++pch)
        for (unsigned col = 0; col < 32; ++col)
          for (unsigned off : {0u, 31u}) check(address(pch, bank, row, col, off));
  std::mt19937_64 rng(81);
  for (unsigned n = 0; n < 100000; ++n) check(rng() & ((1ull << 48) - 1));
  check(0x2D016C);
  printf("CUSTOM_SID_ADDRESS %llu L2_unchanged=1\n", (unsigned long long)checked);
  auto *gpu = new custom_cpu_gpu(*cfg, ctx);
  ctx->the_gpgpusim->g_the_gpu = gpu;
  gpu->gpu_sim_cycle = gpu->gpu_tot_sim_cycle = 0;
  for (bool write : {false, true}) {
    custom_dram_sid_test::column_pair(mc, gpu, 4, write, 5);
    custom_dram_sid_test::column_pair(mc, gpu, 1, write, 2);
    custom_dram_sid_test::column_pair(mc, gpu, 16, write, 2);
  }
  custom_dram_sid_test::row_conflict(mc, gpu);
  custom_dram_sid_test::queued_banks(cfg, gpu);
  puts("CUSTOM_SID_TEST passed");
}
