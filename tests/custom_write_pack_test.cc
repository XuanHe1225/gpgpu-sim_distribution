#include "gpgpu-sim/local_interconnect.h"
#include <cassert>

static inct_config config(bool writes, unsigned capacity = 16) {
  inct_config c{};
  c.in_buffer_limit = 32;
  c.out_buffer_limit = capacity;
  c.subnets = 2;
  c.arbiter_algo = PERFECT;
  c.grant_cycles = 1;
  c.custom_icnt_sector_pack = true;
  c.custom_icnt_write_sector_pack = writes;
  return c;
}

static custom_sector_transfer::Sector sector(unsigned i, bool write = true) {
  custom_sector_transfer::Sector s;
  s.request_known = true;
  s.ordinary_write = write;
  s.ordinary_read = !write;
  s.eligible = !write;
  s.address = 4096 + 32 * i;
  s.instruction = 42;
  s.shader = s.warp = 0;
  return s;
}

int main() {
  int payload[4] = {};
  for (bool enabled : {false, true}) {
    xbar_router r(0, REQ_NET, 1, 1, config(enabled));
    for (unsigned i = 0; i < 4; ++i) r.Push(0, 1, &payload[i], 40, sector(i));
    r.Advance();
    const unsigned expected = enabled ? 4 : 1;
    for (unsigned i = 0; i < expected; ++i) assert(r.Pop(1) == &payload[i]);
    assert(!r.Pop(1));
    assert(r.custom_transfers.sectors == 0);
    if (enabled) assert(r.custom_write_transfers.groups == 1 && r.custom_write_transfers.sectors == 4);
  }
  // Capacity remains per object; preserve every pointer and FIFO order.
  xbar_router limited(0, REQ_NET, 1, 1, config(true, 2));
  for (unsigned i = 0; i < 4; ++i) limited.Push(0, 1, &payload[i], 40, sector(i));
  limited.Advance();
  assert(limited.Pop(1) == &payload[0] && limited.Pop(1) == &payload[1] && !limited.Pop(1));
  limited.Advance();
  assert(limited.Pop(1) == &payload[2] && limited.Pop(1) == &payload[3] && !limited.Pop(1));
  assert(limited.custom_write_transfers.groups == 2 && limited.custom_write_transfers.sectors == 4);
  for (unsigned mismatch = 0; mismatch < 5; ++mismatch) {
    xbar_router r(0, REQ_NET, 1, 1, config(true));
    auto other = sector(1);
    if (mismatch == 0) ++other.instruction;
    if (mismatch == 1) other.address += 128;
    if (mismatch == 2) other = sector(0); // duplicate sector
    if (mismatch == 3) other = sector(1, false); // read cannot join write
    if (mismatch == 4) other.ordinary_write = false; // unknown is not promoted
    r.Push(0, 1, &payload[0], 40, sector(0));
    r.Push(0, 1, &payload[1], 40, other);
    r.Advance();
    assert(r.Pop(1) == &payload[0] && !r.Pop(1));
    r.Advance();
    assert(r.Pop(1) == &payload[1] && !r.Pop(1));
  }
  xbar_router reads(0, REQ_NET, 1, 1, config(true));
  for (unsigned i = 0; i < 4; ++i) reads.Push(0, 1, &payload[i], 8, sector(i, false));
  reads.Advance();
  for (unsigned i = 0; i < 4; ++i) assert(reads.Pop(1) == &payload[i]);
  assert(reads.custom_transfers.groups == 1 && reads.custom_transfers.sectors == 4);
  assert(reads.custom_write_transfers.sectors == 0);
  puts("write packing conserves objects, order, capacity, and read counters");
}
