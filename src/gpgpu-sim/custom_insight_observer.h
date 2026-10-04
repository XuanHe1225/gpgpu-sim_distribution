#pragma once
// Passive, bounded-memory online reductions. Counts are per 10k core cycles.
// Request residence uses actual core time, never memcpy's tag-replacement clock.
#include "custom_insight_address.h"
#include "mem_fetch.h"
#include <algorithm>
#include <bitset>
#include <cstring>
#include <map>
#include <set>
#include <tuple>
#include <vector>

namespace custom_insight {
enum Metric { INJECT, INJECT_BYTES, RETURNED, MC_ADMIT, MC_BYTES, RD_BYTES,
  WR_BYTES, ACT, PRE, SELECTED, ROW_HIT, MERGED, REQUEST_CYCLES,
  WARP_CYCLES, L2_INPUT_SUM, L2_INPUT_FULL, L2_DRAM_SUM, L2_DRAM_FULL,
  ROP_SUM, L2_SAMPLES, MC_QUEUE_SUM, MC_QUEUE_FULL, MC_SAMPLES,
  CANDIDATES, SAME_ROW_CANDIDATES, SCHEDULER_CALLS, CAP_STALL,
  WEIGHT_REQUEST_PEAK, WEIGHT_WARP_PEAK, MC_QUEUE_MAX, L2_INPUT_MAX,
  L2_DRAM_MAX, L2_HIT, L2_PENDING, L2_MISS, L2_SECTOR_MISS, L2_RETRY,
  STATE_CYCLES=64 };
constexpr U bin_width=10000, window_start=40000, window_end=90000;
using Key=std::tuple<U,unsigned,unsigned,unsigned>;
struct Request { U birth=0,since=0,injected=0; unsigned state=0,pch=0,op=2,sm=108; bool sent=false; };
struct Instruction { unsigned sm,warp; U issued; };
struct Local { U warps=0,bytes=0,sectors=0,lines=0,transactions=0; };
struct Gauge { U since=0; unsigned n=0; };
struct Example { U uid,pc,sm,warp,lane,before,after,pch,bank,row,col; };
struct Observer {
  std::map<Key,U> counts;
  std::unordered_map<U,Request> requests;
  std::unordered_map<U,Instruction> instructions;
  std::unordered_map<U,U> cohort_instructions;
  std::map<std::pair<unsigned,unsigned>,U> cohort_count;
  std::map<std::pair<U,U>,std::pair<unsigned,U>> cohort_completed;
  std::map<std::pair<unsigned,unsigned>,unsigned> warp_insts;
  std::map<std::tuple<U,unsigned,unsigned>,U> bank_rd;
  std::map<std::pair<U,unsigned>,std::bitset<80>> rd_ticks;
  std::array<Local,2> local{};
  std::array<bool,4> sampled{};
  std::vector<Example> examples;
  std::map<U,std::array<U,12>> witnesses;
  std::array<U,2> latency_count{},latency_sum{},latency_max{};
  std::array<U,108> sm_request_area{},sm_window_peak{};
  Gauge weight_requests,weight_warps;
  U errors=0,last_cycle=0,weight_ordinal=0;
  // Equal-work prefix endpoints: cumulative completed B-copy ordinals.
  std::map<U,U> completed_prefix;

  void add(U c,unsigned p,unsigned op,unsigned metric,U value=1) {
    counts[{c/bin_width,p,op,metric}]+=value;
  }
  void peak(U c,unsigned p,unsigned op,unsigned metric,U value) {
    auto &v=counts[{c/bin_width,p,op,metric}]; v=std::max(v,value);
  }
  void area(U lo,U hi,unsigned p,unsigned op,unsigned metric,U n=1) {
    assert(hi>=lo);
    while(lo<hi) { U end=std::min(hi,(lo/bin_width+1)*bin_width);
      add(lo,p,op,metric,(end-lo)*n); lo=end; }
  }
  void gauge(Gauge &g,int delta,U c,unsigned metric,unsigned max_metric) {
    area(g.since,c,80,1,metric,g.n);
    for(U b=g.since/bin_width; b<=c/bin_width; ++b)
      peak(b*bin_width,80,1,max_metric,g.n);
    assert(delta>=0 || g.n>=unsigned(-delta)); g.n+=delta; g.since=c;
    peak(c,80,1,max_metric,g.n);
  }
  void instruction(const char *event,U c,const warp_inst_t &inst,unsigned sm) {
    if(!enabled() || !inst.m_is_ldgsts || !inst.active_count()) return;
    last_cycle=std::max(last_cycle,c);
    const U uid=inst.get_uid();
    if(std::strcmp(event,"complete")==0) {
      auto co=cohort_instructions.find(uid);
      if(co!=cohort_instructions.end()) {
        const U ordinal=co->second;
        for(U target: {512ull,1024ull,1536ull}) if(ordinal<=target) {
          auto &v=cohort_completed[{0,target}]; ++v.first; v.second=std::max(v.second,c);
        }
        cohort_instructions.erase(co);
      }
      auto it=instructions.find(uid); if(it==instructions.end()) return;
      const auto key=std::make_pair(it->second.sm,it->second.warp);
      auto w=warp_insts.find(key); assert(w!=warp_insts.end() && w->second);
      if(--w->second==0) { warp_insts.erase(w); gauge(weight_warps,-1,c,WARP_CYCLES,WEIGHT_WARP_PEAK); }
      ++weight_ordinal;
      if(weight_ordinal%10000==0) completed_prefix[weight_ordinal]=c;
      instructions.erase(it); return;
    }
    if(std::strcmp(event,"issue")) return;
    std::set<U> sectors,lines,words;
    unsigned op=2;
    for(unsigned lane=0;lane<inst.warp_size();++lane) if(inst.active(lane)) {
      const U a=inst.get_addr(lane); const unsigned operand_id=operand(a);
      if(op==2) op=operand_id;
      if(operand_id!=op || op>=2 || inst.data_size!=16 || a%16) { ++errors; return; }
      words.insert(a/16); sectors.insert(a/32); lines.insert(a/128);
    }
    if(op>=2) { ++errors; return; }
    auto &v=local[op]; ++v.warps; v.bytes+=inst.active_count()*inst.data_size;
    v.sectors+=sectors.size(); v.lines+=lines.size(); v.transactions+=inst.accessq_count();
    if(words.size()!=inst.active_count()) ++errors;
    if(op==1) {
      if(!instructions.emplace(uid,Instruction{sm,inst.dynamic_warp_id(),c}).second) ++errors;
      auto &n=warp_insts[{sm,inst.dynamic_warp_id()}];
      if(n++==0) gauge(weight_warps,1,c,WARP_CYCLES,WEIGHT_WARP_PEAK);
      auto cta=inst.get_cuda_cta_id(); unsigned w=inst.warp_id();
      if(cta.x<8 && cta.y==0) cohort_instructions[uid]=++cohort_count[{cta.x,w}];
      if(cta.x==0 && cta.y==0 && cta.z==0 && w<4 && !sampled[w]) {
        sampled[w]=true;
        for(unsigned lane=0;lane<inst.warp_size();++lane) if(inst.active(lane)) {
          U a=inst.get_addr(lane); auto d=decode(a);
          examples.push_back({uid,inst.pc,sm,w,lane,unmap_weight(a,settings().base,settings().mode),a,
                              d.pch,d.bank,d.row%8192,d.col});
        }
      }
    }
  }
  void state(mem_fetch *mf,unsigned state,U c) {
    if(!enabled() && !settings().cap) return;
    const U uid=mf->get_request_uid();
    if(mf->get_access_type()==GLOBAL_ACC_R && mf->get_sid()<108) {
      if(state==IN_ICNT_TO_MEM) credits().inject(uid,mf->get_sid());
      if(state==IN_SHADER_LDST_RESPONSE_FIFO) credits().reply(uid);
    }
    if(!enabled()) return;
    auto it=requests.find(uid); if(it==requests.end()) return;
    auto &r=it->second; last_cycle=std::max(last_cycle,c);
    if(state!=r.state) { area(r.since,c,r.pch,r.op,STATE_CYCLES+r.state); r.since=c; r.state=state; }
    if(state==IN_ICNT_TO_MEM) {
      if(r.sent) { ++errors; return; }
      r.sent=true; r.injected=c; add(c,r.pch,r.op,INJECT); add(c,r.pch,r.op,INJECT_BYTES,mf->get_data_size());
      if(r.op==1) gauge(weight_requests,1,c,REQUEST_CYCLES,WEIGHT_REQUEST_PEAK);
      if(r.sm<108 && c>=window_start && c<window_end)
        sm_window_peak[r.sm]=std::max(sm_window_peak[r.sm],U(credits().counts[r.sm]));
      if(r.op==1 && c>=window_start && c<window_end && witnesses.size()<128)
        witnesses[uid]={uid,mf->get_inst().get_uid(),r.sm,mf->get_wid(),r.pch,mf->get_tlx_addr().bk,
          mf->get_tlx_addr().row,mf->get_addr(),c,0,0,0};
    }
    if(state==IN_PARTITION_MC_INTERFACE_QUEUE) { add(c,r.pch,r.op,MC_ADMIT); add(c,r.pch,r.op,MC_BYTES,mf->get_data_size()); }
    if(state==IN_SHADER_LDST_RESPONSE_FIFO && r.sent) {
      add(c,r.pch,r.op,RETURNED); r.sent=false;
      if(r.op==1) gauge(weight_requests,-1,c,REQUEST_CYCLES,WEIGHT_REQUEST_PEAK);
      if(r.sm<108) sm_request_area[r.sm]+=std::min(c,window_end)>std::max(r.injected,window_start)
        ? std::min(c,window_end)-std::max(r.injected,window_start):0;
      if(r.injected>=window_start && r.injected<window_end && r.op<2) {
        ++latency_count[r.op]; latency_sum[r.op]+=c-r.injected;
        latency_max[r.op]=std::max(latency_max[r.op],c-r.injected);
      }
      auto witness=witnesses.find(uid); if(witness!=witnesses.end()) witness->second[11]=c;
    }
  }
  void request(const char *event,U c,mem_fetch *mf,const char *detail,U tick) {
    if(!enabled() || !mf) return;
    const U uid=mf->get_request_uid(); const auto d=mf->get_tlx_addr();
    const unsigned op=operand(mf->get_addr()), p=d.chip;
    if(p>=80) { ++errors; return; } last_cycle=std::max(last_cycle,c);
    if(std::strcmp(event,"create")==0 && mf->get_access_type()==GLOBAL_ACC_R)
      if(!requests.emplace(uid,Request{c,c,0,0,p,op,mf->get_sid(),false}).second) ++errors;
    if(std::strcmp(event,"RD")==0 || std::strcmp(event,"WR")==0) {
      bool read=event[0]=='R'; add(c,p,op,read?RD_BYTES:WR_BYTES,32);
      if(read) {
        bank_rd[{c/bin_width,p,d.bk}]+=32;
        if(c>=window_start && c<window_end) { rd_ticks[{tick,op}].set(p); rd_ticks[{tick,3}].set(p); }
        auto it=witnesses.find(uid); if(it!=witnesses.end()) {it->second[9]=c;it->second[10]=tick;}
      }
    } else if(std::strcmp(event,"ACT")==0) add(c,p,op,ACT);
    else if(std::strcmp(event,"PRE")==0) add(c,p,op,PRE);
    else if(std::strcmp(event,"schedule")==0) {add(c,p,op,SELECTED); if(std::strcmp(detail,"row_hit")==0)add(c,p,op,ROW_HIT);}
    else if(std::strcmp(event,"merge")==0) add(c,p,op,MERGED);
    else if(std::strcmp(event,"l2")==0) {
      if(std::strcmp(detail,"HIT")==0)add(c,p,op,L2_HIT);
      else if(std::strcmp(detail,"HIT_RESERVED")==0)add(c,p,op,L2_PENDING);
      else if(std::strcmp(detail,"MISS")==0)add(c,p,op,L2_MISS);
      else if(std::strcmp(detail,"SECTOR_MISS")==0)add(c,p,op,L2_SECTOR_MISS);
      else if(std::strcmp(detail,"RESERVATION_FAIL")==0)add(c,p,op,L2_RETRY);
    }
  }
  void release(U uid,U c) {
    if(!enabled()) return;
    auto it=requests.find(uid); if(it==requests.end())return;
    const auto r=it->second; area(r.since,c,r.pch,r.op,STATE_CYCLES+r.state);
    if(r.sent) ++errors; requests.erase(it);
  }
  void l2(U c,unsigned sub,unsigned input,unsigned to_dram,unsigned rop,bool input_full,bool dram_full) {
    if(!enabled() || c<window_start || c>=window_end)return;
    const unsigned p=sub/2; add(c,p,3,L2_SAMPLES);
    add(c,p,3,L2_INPUT_SUM,input); add(c,p,3,L2_INPUT_FULL,input_full);
    add(c,p,3,L2_DRAM_SUM,to_dram); add(c,p,3,L2_DRAM_FULL,dram_full); add(c,p,3,ROP_SUM,rop);
    peak(c,p,3,L2_INPUT_MAX,input); peak(c,p,3,L2_DRAM_MAX,to_dram);
  }
  void mc(U c,unsigned p,unsigned pending,bool full) {
    if(!enabled() || c<window_start || c>=window_end)return;
    add(c,p,3,MC_SAMPLES);add(c,p,3,MC_QUEUE_SUM,pending);add(c,p,3,MC_QUEUE_FULL,full);peak(c,p,3,MC_QUEUE_MAX,pending);
  }
  void scheduler(U c,unsigned p,unsigned n,unsigned hits) {
    if(!enabled() || c<window_start || c>=window_end || !n)return;
    add(c,p,3,SCHEDULER_CALLS);add(c,p,3,CANDIDATES,n);add(c,p,3,SAME_ROW_CANDIDATES,hits);
  }
  void gate(U c) { if(enabled())add(c,80,3,CAP_STALL); }
  void print(U c) {
    if(!enabled())return;
    gauge(weight_requests,0,c,REQUEST_CYCLES,WEIGHT_REQUEST_PEAK);
    gauge(weight_warps,0,c,WARP_CYCLES,WEIGHT_WARP_PEAK);
    const std::string root=settings().report;
    auto open=[&](const char *suffix) {const auto p=root+suffix;FILE *f=std::fopen(p.c_str(),"w");if(!f){std::perror(p.c_str());std::abort();}return f;};
    FILE *f=open("-bins.csv"); std::fprintf(f,"bin,pch,operand,metric,value\n");
    for(auto &kv:counts)std::fprintf(f,"%llu,%u,%u,%u,%llu\n",std::get<0>(kv.first),std::get<1>(kv.first),std::get<2>(kv.first),std::get<3>(kv.first),kv.second);
    std::fclose(f); f=open("-rd.csv");std::fprintf(f,"dram_tick,operand,pch_mask\n");
    for(auto &kv:rd_ticks)std::fprintf(f,"%llu,%u,%s\n",kv.first.first,kv.first.second,kv.second.to_string().c_str());
    std::fclose(f); f=open("-bank.csv");std::fprintf(f,"bin,pch,bank,rd_bytes\n");
    for(auto &kv:bank_rd)std::fprintf(f,"%llu,%u,%u,%llu\n",std::get<0>(kv.first),std::get<1>(kv.first),std::get<2>(kv.first),kv.second);
    std::fclose(f);f=open("-examples.csv");std::fprintf(f,"uid,pc,sm,warp,lane,original,address,pch,bank,row,col\n");
    for(auto &e:examples)std::fprintf(f,"%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu\n",e.uid,e.pc,e.sm,e.warp,e.lane,e.before,e.after,e.pch,e.bank,e.row,e.col);
    std::fclose(f);f=open("-witness.csv");std::fprintf(f,"request,inst,sm,warp,pch,bank,row,address,injection,rd,dram_tick,response\n");
    for(auto &kv:witnesses) {for(unsigned i=0;i<12;++i)std::fprintf(f,i?",%llu":"%llu",kv.second[i]);std::fprintf(f,"\n");}
    std::fclose(f);f=open("-prefix.csv");std::fprintf(f,"completed_weight_copies,cycle\n");
    for(auto &kv:completed_prefix)std::fprintf(f,"%llu,%llu\n",kv.first,kv.second);
    std::fclose(f);f=open("-cohort.csv");std::fprintf(f,"copies_per_warp,completed_copies,cycle\n");
    for(auto &kv:cohort_completed)std::fprintf(f,"%llu,%u,%llu\n",kv.first.second,kv.second.first,kv.second.second);
    std::fclose(f);f=open("-sm.csv");std::fprintf(f,"sm,window_request_cycles,window_request_peak,whole_request_peak\n");
    for(unsigned sm=0;sm<108;++sm)std::fprintf(f,"%u,%llu,%llu,%u\n",sm,sm_request_area[sm],sm_window_peak[sm],credits().peaks[sm]);
    std::fclose(f); f=open("-summary.json");
    std::fprintf(f,"{\"cycle\":%llu,\"errors\":%llu,\"pending_requests\":%zu,\"pending_instructions\":%zu,\"pending_credits\":%zu,\"mode\":%u,\"cap\":%u,\"local\":[",c,errors,requests.size(),instructions.size(),credits().pending(),settings().mode,settings().cap);
    for(unsigned i=0;i<2;++i){auto v=local[i];std::fprintf(f,"%s{\"warps\":%llu,\"bytes\":%llu,\"sectors\":%llu,\"lines\":%llu,\"coalescer_transactions\":%llu,\"injection_cohort_count\":%llu,\"injection_cohort_latency_sum\":%llu,\"injection_cohort_latency_max\":%llu}",i?",":"",v.warps,v.bytes,v.sectors,v.lines,v.transactions,latency_count[i],latency_sum[i],latency_max[i]);}
    std::fprintf(f,"]}\n");std::fclose(f);
  }
};
inline Observer &observer() {static Observer o; return o;}
}
