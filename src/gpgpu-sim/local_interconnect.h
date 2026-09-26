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

#ifndef _LOCAL_INTERCONNECT_HPP_
#define _LOCAL_INTERCONNECT_HPP_

#include <iostream>
#include <map>
#include <memory>
#include "custom_icnt_queue_observation.h"
#include "custom_sector_transfer.h"
#include <queue>
#include <vector>
using namespace std;

enum Interconnect_type { REQ_NET = 0, REPLY_NET = 1 };

enum Arbiteration_type {
  NAIVE_RR = 0,  //
  iSLIP,
  PERFECT
};

struct inct_config {
  bool custom_icnt_round_robin = false;
  bool custom_icnt_perfect_fair = false;
  bool custom_icnt_sector_pack = false;
  unsigned custom_icnt_return_width = 1;
  // config for local interconnect
  unsigned in_buffer_limit;
  unsigned out_buffer_limit;
  unsigned subnets;
  Arbiteration_type arbiter_algo;
  unsigned verbose;
  unsigned grant_cycles;
};

class xbar_router {
 public:
  xbar_router(unsigned router_id, enum Interconnect_type m_type,
              unsigned n_shader, unsigned n_mem,
              const struct inct_config& m_localinct_config);
  ~xbar_router();
  void Push(unsigned input_deviceID, unsigned output_deviceID, void* data,
            unsigned int size);
  void Push(unsigned input_deviceID, unsigned output_deviceID, void* data,
            unsigned int size, const custom_sector_transfer::Sector &sector);
  void* Pop(unsigned ouput_deviceID);
  void* Peek(unsigned output_deviceID) const;
  void Advance();

  bool Busy() const;
  bool Has_Buffer_In(unsigned input_deviceID, unsigned size,
                     bool update_counter = false);
  bool Has_Buffer_Out(unsigned output_deviceID, unsigned size);
  bool Has_Packet(unsigned output_deviceID) const;

  // some stats
  unsigned long long cycles;
  unsigned long long conflicts;
  unsigned long long conflicts_util;
  unsigned long long cycles_util;
  unsigned long long reqs_util;
  unsigned long long out_buffer_full;
  unsigned long long out_buffer_util;
  unsigned long long in_buffer_full;
  unsigned long long in_buffer_util;
  unsigned long long packets_num;
  custom_sector_transfer::Counters custom_transfers;

 private:
  std::unique_ptr<custom_icnt_queue::Census> custom_queue_census;
  void custom_queue_depart(unsigned input,unsigned output);
  void iSLIP_Advance();
  void RR_Advance();
  void Perfect_Advance();
  void Perfect_Fair_Advance();

  struct Packet {
    Packet(void* m_data, unsigned m_output_deviceID,
           const custom_sector_transfer::Sector &info = {}) : sector(info) {
      data = m_data;
      output_deviceID = m_output_deviceID;
    }
    void* data;
    unsigned output_deviceID;
    custom_sector_transfer::Sector sector;
  };
  vector<queue<Packet> > in_buffers;
  vector<queue<Packet> > out_buffers;
  unsigned _n_shader, _n_mem, total_nodes;
  unsigned in_buffer_limit, out_buffer_limit;
  vector<unsigned> next_node;  // used for iSLIP arbit
  unsigned next_node_id;       // used for RR arbit
  unsigned m_id;
  enum Interconnect_type router_type;
  unsigned active_in_buffers, active_out_buffers;
  Arbiteration_type arbit_type;
  unsigned verbose;

  unsigned grant_cycles;
  unsigned grant_cycles_count;
  bool custom_icnt_round_robin;
  bool custom_icnt_sector_pack = false;
  bool custom_icnt_perfect_fair = false;
  // Tick-start head lists, sorted by input ID. No input FIFO tail is searched.
  vector<unsigned> custom_fair_head, custom_fair_tail, custom_fair_link;
  struct FairStats {
    unsigned long long grants = 0, sectors = 0, wait_ticks = 0;
    unsigned long long consecutive_wait = 0, max_wait = 0;
  };
  vector<FairStats> custom_fair_stats;

  friend class LocalInterconnect;
};

class LocalInterconnect {
 public:
  LocalInterconnect(const struct inct_config& m_localinct_config);
  ~LocalInterconnect();
  static LocalInterconnect* New(const struct inct_config& m_inct_config);
  void CreateInterconnect(unsigned n_shader, unsigned n_mem);

  // node side functions
  void Init();
  void Push(unsigned input_deviceID, unsigned output_deviceID, void* data,
            unsigned int size);
  void Push(unsigned input_deviceID, unsigned output_deviceID, void* data,
            unsigned int size, const custom_sector_transfer::Sector &sector);
  void* Pop(unsigned ouput_deviceID);
  void* Peek(unsigned output_deviceID) const;
  void Advance();
  bool Busy() const;
  bool HasBuffer(unsigned deviceID, unsigned int size) const;
  bool HasPacket(unsigned deviceID) const;
  void DisplayStats() const;
  void DisplayOverallStats() const;
  unsigned GetFlitSize() const;

  void DisplayState(FILE* fp) const;

 protected:
  const inct_config& m_inct_config;

  unsigned n_shader, n_mem;
  unsigned n_subnets;
  vector<xbar_router*> net;
};

#endif
