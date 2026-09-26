// Copyright (c) 2019, Mahmoud Khairy
// Purdue University
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//
// Redistributions of source code must retain the above copyright notice, this
// list of conditions and the following disclaimer.
// Redistributions in binary form must reproduce the above copyright notice,
// this list of conditions and the following disclaimer in the documentation
// and/or other materials provided with the distribution. Neither the name of
// The University of British Columbia nor the names of its contributors may be
// used to endorse or promote products derived from this software without
// specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
// AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
// ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
// LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
// CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
// SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
// INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
// CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
// ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
// POSSIBILITY OF SUCH DAMAGE.

#include <algorithm>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <utility>

#include "local_interconnect.h"
#include "mem_fetch.h"
#include "custom_region_observation.h"
#include "custom_dram_progress.h"

xbar_router::xbar_router(unsigned router_id, enum Interconnect_type m_type,
                         unsigned n_shader, unsigned n_mem,
                         const struct inct_config& m_localinct_config) {
  m_id = router_id;
  router_type = m_type;
  _n_mem = n_mem;
  _n_shader = n_shader;
  total_nodes = n_shader + n_mem;
  verbose = m_localinct_config.verbose;
  grant_cycles = m_localinct_config.grant_cycles;
  custom_icnt_round_robin = m_localinct_config.custom_icnt_round_robin;
  custom_icnt_sector_pack = m_localinct_config.custom_icnt_sector_pack;
  custom_icnt_perfect_fair = m_localinct_config.custom_icnt_perfect_fair;
  custom_icnt_perfect_fair_eligible =
      m_localinct_config.custom_icnt_perfect_fair_eligible;
  if (custom_icnt_perfect_fair && custom_icnt_perfect_fair_eligible)
    throw std::invalid_argument("custom fair modes are mutually exclusive");
  if ((custom_icnt_perfect_fair || custom_icnt_perfect_fair_eligible) &&
      (m_localinct_config.arbiter_algo != PERFECT || m_localinct_config.subnets != 2 ||
       n_shader == 0 || n_mem == 0))
    throw std::invalid_argument("custom fair arbitration requires local PERFECT and two nonempty subnets");
  if (custom_icnt_perfect_fair || custom_icnt_perfect_fair_eligible) {
    custom_fair_head.resize(total_nodes);
    custom_fair_tail.resize(total_nodes);
    custom_fair_link.resize(total_nodes);
    custom_fair_stats.resize(total_nodes);
  }
  if (custom_icnt_perfect_fair_eligible)
    custom_legacy_stats.resize(total_nodes);
  const unsigned width = m_localinct_config.custom_icnt_return_width;
  if ((width != 1 && width != 4) || (width == 4 && !custom_icnt_sector_pack) ||
      (custom_icnt_sector_pack && (m_localinct_config.arbiter_algo != PERFECT ||
                                  m_localinct_config.subnets != 2)))
    throw std::invalid_argument("custom sector transfers require local PERFECT, two subnets, packing for width 4");
  grant_cycles_count = m_localinct_config.grant_cycles;
  in_buffers.resize(total_nodes);
  out_buffers.resize(total_nodes);
  next_node.resize(total_nodes, 0);
  in_buffer_limit = m_localinct_config.in_buffer_limit;
  out_buffer_limit = m_localinct_config.out_buffer_limit;
  arbit_type = m_localinct_config.arbiter_algo;
  next_node_id = 0;
  if (m_type == REQ_NET) {
    active_in_buffers = n_shader;
    active_out_buffers = n_mem;
  } else if (m_type == REPLY_NET) {
    active_in_buffers = n_mem;
    active_out_buffers = n_shader;
  }

  if(custom_icnt_queue::enabled_flag() && m_type==REQ_NET) {
    if(arbit_type!=iSLIP || m_localinct_config.subnets!=2 || !custom_region::enabled_flag())
      throw std::invalid_argument("custom_icnt_queue_observation requires region observation, iSLIP and two subnets");
    custom_queue_census.reset(new custom_icnt_queue::Census(n_shader,n_mem));
  }
  cycles = 0;
  conflicts = 0;
  out_buffer_full = 0;
  in_buffer_full = 0;
  out_buffer_util = 0;
  in_buffer_util = 0;
  packets_num = 0;
  conflicts_util = 0;
  cycles_util = 0;
  reqs_util = 0;
}

xbar_router::~xbar_router() {}

void xbar_router::Push(unsigned input_deviceID, unsigned output_deviceID,
                       void* data, unsigned int size) {
  Push(input_deviceID, output_deviceID, data, size, {});
}

void xbar_router::Push(unsigned input_deviceID, unsigned output_deviceID,
                       void* data, unsigned int size,
                       const custom_sector_transfer::Sector &sector) {
  assert(input_deviceID < total_nodes);
  if (custom_icnt_perfect_fair || custom_icnt_perfect_fair_eligible) {
    assert((input_deviceID < _n_shader) == (router_type == REQ_NET));
    assert(output_deviceID < total_nodes);
    assert((output_deviceID >= _n_shader) == (router_type == REQ_NET));
  }
  in_buffers[input_deviceID].push(Packet(data, output_deviceID, sector));
  packets_num++;
  custom_dram_progress::network_packet(data,m_id,0);
  if(custom_queue_census) {
    assert(output_deviceID>=_n_shader);
    custom_queue_census->push(input_deviceID,output_deviceID-_n_shader);
    custom_region::observations().icnt_packet(output_deviceID-_n_shader,true);
  }
}

void* xbar_router::Pop(unsigned ouput_deviceID) {
  assert(ouput_deviceID < total_nodes);
  void* data = NULL;

  if (!out_buffers[ouput_deviceID].empty()) {
    data = out_buffers[ouput_deviceID].front().data;
    out_buffers[ouput_deviceID].pop();
    custom_dram_progress::network_packet(data,m_id,2);
  }

  return data;
}

void* xbar_router::Peek(unsigned output_deviceID) const {
  assert(output_deviceID < total_nodes);
  return out_buffers[output_deviceID].empty() ? nullptr : out_buffers[output_deviceID].front().data;
}

bool xbar_router::Has_Buffer_In(unsigned input_deviceID, unsigned size,
                                bool update_counter) {
  assert(input_deviceID < total_nodes);

  bool has_buffer =
      (in_buffers[input_deviceID].size() + size <= in_buffer_limit);
  if (update_counter && !has_buffer) in_buffer_full++;

  return has_buffer;
}

bool xbar_router::Has_Buffer_Out(unsigned output_deviceID, unsigned size) {
  return (out_buffers[output_deviceID].size() + size <= out_buffer_limit);
}

void xbar_router::custom_queue_depart(unsigned input,unsigned output) {
  custom_dram_progress::network_packet(in_buffers[input].front().data,m_id,1);
  if(custom_queue_census) {
    custom_queue_census->pop(input,output-_n_shader);
    custom_region::observations().icnt_packet(output-_n_shader,false);
  }
}

void xbar_router::Advance() {
  const bool queue_active=custom_queue_census && custom_region::observations().has_open_window();
  const bool queue_sample=queue_active && custom_icnt_queue::sample_tick(cycles);
  const auto queue_tick=cycles,queue_prior_grants=reqs_util;
  unsigned queue_heads=0;
  custom_icnt_queue::Sample queue_snapshot;
  std::vector<int> queue_fronts;
  std::vector<unsigned> queue_destinations;
  if(queue_active) {
    queue_fronts.assign(_n_shader,-1);
    std::vector<bool> ready(_n_mem,false),heads(_n_mem,false);
    for(unsigned d=0;d<_n_mem;++d) ready[d]=Has_Buffer_Out(d+_n_shader,1);
    for(unsigned i=0;i<_n_shader;++i) {
      assert(in_buffers[i].size()==custom_queue_census->size(i));
      if(!in_buffers[i].empty()) {
        const unsigned d=in_buffers[i].front().output_deviceID-_n_shader;
        queue_fronts[i]=d;if(ready[d]) heads[d]=true;
      }
    }
    queue_heads=std::count(heads.begin(),heads.end(),true);
    if(queue_sample) {
      queue_snapshot=custom_queue_census->snapshot(queue_fronts,ready);queue_snapshot.tick=cycles;
      queue_destinations.assign(_n_mem,0);
      for(const auto &row:custom_queue_census->population()) for(unsigned d=0;d<_n_mem;++d) queue_destinations[d]+=row[d];
    }
  }
  const bool sample=custom_region::enabled_flag() && cycles%64==0;
  const auto prior_grants=reqs_util;
  if(sample) {
    unsigned nonempty=0,queued=0,blocked=0,behind=0;
    std::set<unsigned> destinations,ready;
    for(unsigned i=0;i<total_nodes;++i) if(!in_buffers[i].empty()) {
      ++nonempty;queued+=in_buffers[i].size();
      unsigned dest=in_buffers[i].front().output_deviceID;
      destinations.insert(dest);
      if(Has_Buffer_Out(dest,1))ready.insert(dest);
      else {++blocked;behind+=in_buffers[i].size()-1;}
    }
    auto &o=custom_region::observations();
    o.count(1,16,m_id,0);o.count(1,16,m_id,1,nonempty);o.count(1,16,m_id,2,queued);
    o.count(1,16,m_id,3,destinations.size());o.count(1,16,m_id,4,ready.size());
    o.count(1,16,m_id,5,blocked);o.count(1,16,m_id,6,behind);
  }
  if (arbit_type == NAIVE_RR)
    RR_Advance();
  else if (arbit_type == iSLIP)
    iSLIP_Advance();
  else if (arbit_type == PERFECT)
    Perfect_Advance();
  else
    assert(0);
  if(sample) custom_region::observations().count(1,16,m_id,7,reqs_util-prior_grants);
  if(queue_active) {
    const unsigned grants=reqs_util-queue_prior_grants;
    queue_snapshot.grants=grants;
    custom_region::observations().icnt_tick(queue_tick,queue_heads,grants,
      queue_sample?&queue_snapshot:nullptr,queue_fronts,queue_destinations);
  }
}

void xbar_router::Perfect_Advance() {
  if (custom_icnt_perfect_fair) {
    Perfect_Fair_Advance();
    return;
  }
  if (custom_icnt_perfect_fair_eligible) {
    const unsigned input_begin = router_type == REQ_NET ? 0 : _n_shader;
    const unsigned input_end = router_type == REQ_NET ? _n_shader : total_nodes;
    bool has_head = false;
    bool all_heads_eligible = true;
    for (unsigned input = input_begin; input < input_end; ++input) {
      if (in_buffers[input].empty()) continue;
      has_head = true;
      if (!in_buffers[input].front().sector.valid()) {
        all_heads_eligible = false;
        break;
      }
    }
    // A mixed tick stays on the legacy path. This is deliberately conservative:
    // no FIFO tail is inspected and ordinary L1-miss traffic is unchanged.
    if (has_head && all_heads_eligible) {
      ++custom_fair_ticks;
      for (auto &s : custom_legacy_stats) s.consecutive_wait = 0;
      Perfect_Fair_Advance();
      return;
    }
    ++custom_legacy_ticks;
    for (auto &s : custom_fair_stats) s.consecutive_wait = 0;
  }
  bool active = false;
  unsigned reqs = 0;
  for (unsigned i = 0; i < total_nodes; i++) {
    unsigned node_id = (i + next_node_id) % total_nodes;
    if (!in_buffers[node_id].empty()) {
      active = true;
      Packet _packet = in_buffers[node_id].front();
      if (Has_Buffer_Out(_packet.output_deviceID, 1)) {
        out_buffers[_packet.output_deviceID].push(_packet);
        custom_queue_depart(node_id,_packet.output_deviceID);
        in_buffers[node_id].pop();
        ++reqs;
        unsigned transferred = 1;
        unsigned sectors = _packet.sector.valid() ? 1 : 0;
        unsigned used = _packet.sector.bit();
        // Only the immediate prefix can share this scheduling opportunity.
        // All queue entries and downstream L2 accesses remain original sectors.
        if (custom_icnt_sector_pack && router_type == REQ_NET && sectors) {
          for (unsigned n = 1; n < 4 && !in_buffers[node_id].empty(); ++n) {
            const Packet next = in_buffers[node_id].front();
            if (next.output_deviceID != _packet.output_deviceID ||
                !next.sector.matches(_packet.sector, used)) break;
            if (!Has_Buffer_Out(next.output_deviceID, 1)) break;
            out_buffers[next.output_deviceID].push(next);
            custom_queue_depart(node_id, next.output_deviceID);
            in_buffers[node_id].pop();
            used |= next.sector.bit(); ++sectors; ++reqs; ++transferred;
          }
        }
        custom_transfers.add(sectors);
        if (custom_icnt_perfect_fair_eligible) {
          auto &stats = custom_legacy_stats[node_id];
          ++stats.grants;
          stats.sectors += transferred;
          stats.consecutive_wait = 0;
        }
      } else {
        out_buffer_full++;
        if (custom_icnt_perfect_fair_eligible) {
          auto &stats = custom_legacy_stats[node_id];
          ++stats.wait_ticks;
          stats.max_wait = std::max(stats.max_wait, ++stats.consecutive_wait);
        }
        custom_dram_progress::network_packet(_packet.data,m_id,3);
      }
    } else if (custom_icnt_perfect_fair_eligible) {
      custom_legacy_stats[node_id].consecutive_wait = 0;
    }
  }

  next_node_id = (next_node_id + 1) % total_nodes;

  // Observe the unchanged upstream PERFECT transfers. In particular, multiple
  // inputs may reach one output in a tick; FIFO matching bounds do not apply.
  if (active) ++cycles_util;
  reqs_util += reqs;
  for (unsigned i = 0; i < total_nodes; ++i) {
    in_buffer_util += in_buffers[i].size();
    out_buffer_util += out_buffers[i].size();
  }
  ++cycles;
};

void xbar_router::Perfect_Fair_Advance() {
  const unsigned input_begin = router_type == REQ_NET ? 0 : _n_shader;
  const unsigned input_end = router_type == REQ_NET ? _n_shader : total_nodes;
  const unsigned output_begin = router_type == REQ_NET ? _n_shader : 0;
  const unsigned output_end = router_type == REQ_NET ? total_nodes : _n_shader;
  const unsigned none = total_nodes;
  std::fill(custom_fair_head.begin(), custom_fair_head.end(), none);
  bool active = false;
  unsigned reqs = 0;

  // Build destination lists once from the tick-start FIFO heads. O(nodes),
  // instead of probing every input separately for every output.
  for (unsigned input = input_begin; input < input_end; ++input) {
    if (in_buffers[input].empty()) {
      custom_fair_stats[input].consecutive_wait = 0;
      continue;
    }
    active = true;
    const unsigned dest = in_buffers[input].front().output_deviceID;
    if (custom_fair_head[dest] == none) custom_fair_head[dest] = input;
    else custom_fair_link[custom_fair_tail[dest]] = input;
    custom_fair_tail[dest] = input;
    custom_fair_link[input] = none;
  }
  for (unsigned dest = output_begin; dest < output_end; ++dest) {
    const unsigned first = custom_fair_head[dest];
    if (first == none) continue;
    unsigned start = first;
    while (start != none && start < next_node[dest]) start = custom_fair_link[start];
    if (start == none) start = first;
    custom_fair_link[custom_fair_tail[dest]] = first;
    unsigned input = start;
    do {
      auto &stats = custom_fair_stats[input];
      const Packet packet = in_buffers[input].front();
      assert(packet.output_deviceID == dest);
      if (Has_Buffer_Out(dest, 1)) {
        out_buffers[dest].push(packet);
        custom_queue_depart(input, dest);
        in_buffers[input].pop();
        unsigned transferred = 1;
        unsigned sectors = packet.sector.valid() ? 1 : 0;
        unsigned used = packet.sector.bit();
        if (custom_icnt_sector_pack && router_type == REQ_NET && sectors) {
          for (unsigned n = 1; n < 4 && !in_buffers[input].empty(); ++n) {
            const Packet next = in_buffers[input].front();
            if (next.output_deviceID != dest || !next.sector.matches(packet.sector, used)) break;
            if (!Has_Buffer_Out(dest, 1)) break;
            out_buffers[dest].push(next);
            custom_queue_depart(input, dest);
            in_buffers[input].pop();
            used |= next.sector.bit();
            ++sectors; ++transferred;
          }
        }
        custom_transfers.add(sectors);
        reqs += transferred;
        ++stats.grants;
        stats.sectors += transferred;
        stats.consecutive_wait = 0;
        // Independent output cursor, advanced only by a successful grant.
        next_node[dest] = input + 1 == input_end ? input_begin : input + 1;
      } else {
        ++out_buffer_full;
        ++stats.wait_ticks;
        stats.max_wait = std::max(stats.max_wait, ++stats.consecutive_wait);
        custom_dram_progress::network_packet(packet.data, m_id, 3);
      }
      input = custom_fair_link[input];
    } while (input != start);
  }
  if (active) ++cycles_util;
  reqs_util += reqs;
  for (unsigned i = 0; i < total_nodes; ++i) {
    in_buffer_util += in_buffers[i].size();
    out_buffer_util += out_buffers[i].size();
  }
  ++cycles;
}

void xbar_router::RR_Advance() {
  bool active = false;
  vector<bool> issued(total_nodes, false);
  unsigned conflict_sub = 0;
  unsigned reqs = 0;

  for (unsigned i = 0; i < total_nodes; ++i) {
    unsigned node_id = (i + next_node_id) % total_nodes;

    if (!in_buffers[node_id].empty()) {
      active = true;
      Packet _packet = in_buffers[node_id].front();
      // ensure that the outbuffer has space and not issued before in this cycle
      if (Has_Buffer_Out(_packet.output_deviceID, 1)) {
        if (!issued[_packet.output_deviceID]) {
          out_buffers[_packet.output_deviceID].push(_packet);
          custom_queue_depart(node_id,_packet.output_deviceID);
          in_buffers[node_id].pop();
          issued[_packet.output_deviceID] = true;
          reqs++;
        } else
          conflict_sub++;
      } else {
        out_buffer_full++;

        if (issued[_packet.output_deviceID]) conflict_sub++;
      }
    }
  }
  next_node_id = next_node_id + 1;
  next_node_id = (next_node_id % total_nodes);

  conflicts += conflict_sub;
  if (active) {
    conflicts_util += conflict_sub;
    cycles_util++;
    reqs_util += reqs;
  }

  if (verbose) {
    printf("%d : cycle %llu : conflicts = %d\n", m_id, cycles, conflict_sub);
    printf("%d : cycle %llu : passing reqs = %d\n", m_id, cycles, reqs);
  }

  // collect some stats about buffer util
  for (unsigned i = 0; i < total_nodes; ++i) {
    in_buffer_util += in_buffers[i].size();
    out_buffer_util += out_buffers[i].size();
  }

  cycles++;
}

// iSLIP algorithm
// McKeown, Nick. "The iSLIP scheduling algorithm for input-queued switches."
// IEEE/ACM transactions on networking 2 (1999): 188-201.
// https://www.cs.rutgers.edu/~sn624/552-F18/papers/islip.pdf
void xbar_router::iSLIP_Advance() {
  bool active = false;

  unsigned conflict_sub = 0;
  unsigned reqs = 0;

  // calcaulte how many conflicts are there for stats
  std::set<unsigned> input_nodes;
  std::set<unsigned> destination_set;
  for (unsigned i = 0; i < total_nodes; ++i) {
    if (!in_buffers[i].empty()) {
      input_nodes.insert(i);
      unsigned out_node = in_buffers[i].front().output_deviceID;

      if (destination_set.find(out_node) != destination_set.end()) {
        conflict_sub++;
      }
      destination_set.insert(out_node);
      active = true;
    }
  }

  conflicts += conflict_sub;
  if (active) {
    conflicts_util += conflict_sub;
    cycles_util++;
  }
  // do iSLIP
  for (auto dest : destination_set) {
    if (Has_Buffer_Out(dest, 1)) {
      unsigned start_node = next_node[dest];
      // next_node already stores winner + 1. Including that source prevents
      // adjacent contenders from repeatedly skipping the next eligible grant.
      // Keep the original path available for paired disabled regressions.
      auto it = custom_icnt_round_robin
          ? input_nodes.lower_bound(start_node)
          : std::upper_bound(input_nodes.begin(), input_nodes.end(), start_node);
      for (unsigned j = 0; j < input_nodes.size(); j++, it++) {
        if (it == input_nodes.end()) {
          it = input_nodes.begin();
        }
        unsigned node_id = *it;
        assert(!in_buffers[node_id].empty());
        Packet _packet = in_buffers[node_id].front();
        if (_packet.output_deviceID == dest) {
          out_buffers[_packet.output_deviceID].push(_packet);
          custom_queue_depart(node_id,_packet.output_deviceID);
          in_buffers[node_id].pop();
          input_nodes.erase(node_id);  // can only be used once
          if (verbose)
            printf("%d : cycle %llu : send req from %d to %d\n", m_id, cycles,
                   node_id, dest - _n_shader);
          if (grant_cycles_count == 1)
            next_node[dest] = (++node_id % total_nodes);
          if (verbose) {
            for (unsigned k = j + 1; k < total_nodes; ++k) {
              unsigned node_id2 = (k + next_node[dest]) % total_nodes;
              if (!in_buffers[node_id2].empty()) {
                Packet _packet2 = in_buffers[node_id2].front();

                if (_packet2.output_deviceID == dest)
                  printf("%d : cycle %llu : cannot send req from %d to %d\n",
                         m_id, cycles, node_id2, dest - _n_shader);
              }
            }
          }

          reqs++;
          break;
        }
      }
    } else {
      out_buffer_full++;
    }
  }

  if (active) {
    reqs_util += reqs;
  }

  if (verbose)
    printf("%d : cycle %llu : grant_cycles = %d\n", m_id, cycles, grant_cycles);

  if (active && grant_cycles_count == 1)
    grant_cycles_count = grant_cycles;
  else if (active)
    grant_cycles_count--;

  if (verbose) {
    printf("%d : cycle %llu : conflicts = %d\n", m_id, cycles, conflict_sub);
    printf("%d : cycle %llu : passing reqs = %d\n", m_id, cycles, reqs);
  }

  // collect some stats about buffer util
  for (unsigned i = 0; i < total_nodes; ++i) {
    in_buffer_util += in_buffers[i].size();
    out_buffer_util += out_buffers[i].size();
  }

  cycles++;
}

bool xbar_router::Busy() const {
  for (unsigned i = 0; i < total_nodes; ++i) {
    if (!in_buffers[i].empty()) return true;

    if (!out_buffers[i].empty()) return true;
  }
  return false;
}

////////////////////////////////////////////////////
/////////////LocalInterconnect/////////////////////

// assume all the packets are one flit
#define LOCAL_INCT_FLIT_SIZE 40

LocalInterconnect* LocalInterconnect::New(
    const struct inct_config& m_localinct_config) {
  LocalInterconnect* icnt_interface = new LocalInterconnect(m_localinct_config);

  return icnt_interface;
}

LocalInterconnect::LocalInterconnect(
    const struct inct_config& m_localinct_config)
    : m_inct_config(m_localinct_config) {
  n_shader = 0;
  n_mem = 0;
  n_subnets = m_localinct_config.subnets;
}

LocalInterconnect::~LocalInterconnect() {
  for (unsigned i = 0; i < m_inct_config.subnets; ++i) {
    delete net[i];
  }
}

void LocalInterconnect::CreateInterconnect(unsigned m_n_shader,
                                           unsigned m_n_mem) {
  n_shader = m_n_shader;
  n_mem = m_n_mem;

  net.resize(n_subnets);
  for (unsigned i = 0; i < n_subnets; ++i) {
    net[i] = new xbar_router(i, static_cast<Interconnect_type>(i), m_n_shader,
                             m_n_mem, m_inct_config);
  }
}

void LocalInterconnect::Init() {
  // empty
  // there is nothing to do
}

void LocalInterconnect::Push(unsigned input_deviceID, unsigned output_deviceID,
                             void* data, unsigned int size) {
  Push(input_deviceID, output_deviceID, data, size, {});
}

void LocalInterconnect::Push(unsigned input_deviceID, unsigned output_deviceID,
                             void* data, unsigned int size,
                             const custom_sector_transfer::Sector &sector) {
  unsigned subnet;
  if (n_subnets == 1) {
    subnet = 0;
  } else {
    if (input_deviceID < n_shader) {
      subnet = 0;
    } else {
      subnet = 1;
    }
  }

  // it should have free buffer
  // assume all the packets have size of one
  // no flits are implemented
  assert(net[subnet]->Has_Buffer_In(input_deviceID, 1));

  net[subnet]->Push(input_deviceID, output_deviceID, data, size, sector);
}

void* LocalInterconnect::Pop(unsigned ouput_deviceID) {
  // 0-_n_shader-1 indicates reply(network 1), otherwise request(network 0)
  int subnet = 0;
  if (ouput_deviceID < n_shader) subnet = 1;

  return net[subnet]->Pop(ouput_deviceID);
}

void* LocalInterconnect::Peek(unsigned output_deviceID) const {
  return net[output_deviceID < n_shader ? REPLY_NET : REQ_NET]->Peek(output_deviceID);
}

bool xbar_router::Has_Packet(unsigned output_deviceID) const {
  return !out_buffers[output_deviceID].empty();
}

bool LocalInterconnect::HasPacket(unsigned deviceID) const {
  int subnet = (deviceID < n_shader) ? 1 : 0;
  return net[subnet]->Has_Packet(deviceID);
}

void LocalInterconnect::Advance() {
  for (unsigned i = 0; i < n_subnets; ++i) {
    net[i]->Advance();
  }
}

bool LocalInterconnect::Busy() const {
  for (unsigned i = 0; i < n_subnets; ++i) {
    if (net[i]->Busy()) return true;
  }
  return false;
}

bool LocalInterconnect::HasBuffer(unsigned deviceID, unsigned int size) const {
  bool has_buffer = false;

  if ((n_subnets > 1) && deviceID >= n_shader)  // deviceID is memory node
    has_buffer = net[REPLY_NET]->Has_Buffer_In(deviceID, 1, true);
  else
    has_buffer = net[REQ_NET]->Has_Buffer_In(deviceID, 1, true);

  return has_buffer;
}

void LocalInterconnect::DisplayStats() const {
  // Exact read-only totals make conservation auditable without rounded ratios.
  for (unsigned i = 0; i < n_subnets; ++i) {
    printf("custom_icnt_totals subnet=%u injected=%llu granted=%llu cycles=%llu active=%llu busy=%u\n",
           i, net[i]->packets_num, net[i]->reqs_util, net[i]->cycles,
           net[i]->cycles_util, static_cast<unsigned>(net[i]->Busy()));
    net[i]->custom_transfers.print(i == REQ_NET ? "request_xbar" : "reply_xbar");
    if (net[i]->custom_icnt_perfect_fair || net[i]->custom_icnt_perfect_fair_eligible) {
      const unsigned begin = i == REQ_NET ? 0 : n_shader;
      const unsigned end = i == REQ_NET ? n_shader : n_shader + n_mem;
      for (unsigned input = begin; input < end; ++input) {
        const auto &s = net[i]->custom_fair_stats[input];
        printf("custom_icnt_fair subnet=%u input=%u grants=%llu sectors=%llu wait_ticks=%llu max_wait=%llu\n",
               i, input, s.grants, s.sectors, s.wait_ticks, s.max_wait);
        if (net[i]->custom_icnt_perfect_fair_eligible) {
          const auto &legacy = net[i]->custom_legacy_stats[input];
          printf("custom_icnt_legacy subnet=%u input=%u grants=%llu sectors=%llu wait_ticks=%llu max_wait=%llu\n",
                 i, input, legacy.grants, legacy.sectors, legacy.wait_ticks, legacy.max_wait);
        }
      }
      if (net[i]->custom_icnt_perfect_fair_eligible)
        printf("custom_icnt_scope subnet=%u fair_ticks=%llu legacy_ticks=%llu\n",
               i, net[i]->custom_fair_ticks, net[i]->custom_legacy_ticks);
    }
  }
  printf("Req_Network_injected_packets_num = %lld\n",
         net[REQ_NET]->packets_num);
  printf("Req_Network_cycles = %lld\n", net[REQ_NET]->cycles);
  printf("Req_Network_injected_packets_per_cycle = %12.4f \n",
         (float)(net[REQ_NET]->packets_num) / (net[REQ_NET]->cycles));
  printf("Req_Network_conflicts_per_cycle = %12.4f\n",
         (float)(net[REQ_NET]->conflicts) / (net[REQ_NET]->cycles));
  printf("Req_Network_conflicts_per_cycle_util = %12.4f\n",
         (float)(net[REQ_NET]->conflicts_util) / (net[REQ_NET]->cycles_util));
  printf("Req_Bank_Level_Parallism = %12.4f\n",
         (float)(net[REQ_NET]->reqs_util) / (net[REQ_NET]->cycles_util));
  printf("Req_Network_in_buffer_full_per_cycle = %12.4f\n",
         (float)(net[REQ_NET]->in_buffer_full) / (net[REQ_NET]->cycles));
  printf("Req_Network_in_buffer_avg_util = %12.4f\n",
         ((float)(net[REQ_NET]->in_buffer_util) / (net[REQ_NET]->cycles) /
          net[REQ_NET]->active_in_buffers));
  printf("Req_Network_out_buffer_full_per_cycle = %12.4f\n",
         (float)(net[REQ_NET]->out_buffer_full) / (net[REQ_NET]->cycles));
  printf("Req_Network_out_buffer_avg_util = %12.4f\n",
         ((float)(net[REQ_NET]->out_buffer_util) / (net[REQ_NET]->cycles) /
          net[REQ_NET]->active_out_buffers));

  printf("\n");
  printf("Reply_Network_injected_packets_num = %lld\n",
         net[REPLY_NET]->packets_num);
  printf("Reply_Network_cycles = %lld\n", net[REPLY_NET]->cycles);
  printf("Reply_Network_injected_packets_per_cycle =  %12.4f\n",
         (float)(net[REPLY_NET]->packets_num) / (net[REPLY_NET]->cycles));
  printf("Reply_Network_conflicts_per_cycle =  %12.4f\n",
         (float)(net[REPLY_NET]->conflicts) / (net[REPLY_NET]->cycles));
  printf(
      "Reply_Network_conflicts_per_cycle_util = %12.4f\n",
      (float)(net[REPLY_NET]->conflicts_util) / (net[REPLY_NET]->cycles_util));
  printf("Reply_Bank_Level_Parallism = %12.4f\n",
         (float)(net[REPLY_NET]->reqs_util) / (net[REPLY_NET]->cycles_util));
  printf("Reply_Network_in_buffer_full_per_cycle = %12.4f\n",
         (float)(net[REPLY_NET]->in_buffer_full) / (net[REPLY_NET]->cycles));
  printf("Reply_Network_in_buffer_avg_util = %12.4f\n",
         ((float)(net[REPLY_NET]->in_buffer_util) / (net[REPLY_NET]->cycles) /
          net[REPLY_NET]->active_in_buffers));
  printf("Reply_Network_out_buffer_full_per_cycle = %12.4f\n",
         (float)(net[REPLY_NET]->out_buffer_full) / (net[REPLY_NET]->cycles));
  printf("Reply_Network_out_buffer_avg_util = %12.4f\n",
         ((float)(net[REPLY_NET]->out_buffer_util) / (net[REPLY_NET]->cycles) /
          net[REPLY_NET]->active_out_buffers));
}

void LocalInterconnect::DisplayOverallStats() const {}

unsigned LocalInterconnect::GetFlitSize() const { return LOCAL_INCT_FLIT_SIZE; }

void LocalInterconnect::DisplayState(FILE* fp) const {
  fprintf(fp, "GPGPU-Sim uArch: ICNT:Display State: Under implementation\n");
}
