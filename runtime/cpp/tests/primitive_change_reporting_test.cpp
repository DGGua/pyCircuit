#include <cassert>

#include "pyc_async_fifo.hpp"
#include "pyc_byte_mem.hpp"
#include "pyc_cdc_sync.hpp"
#include "pyc_primitives.hpp"
#include "pyc_sync_mem.hpp"
#include "pyc_vec.hpp"

using namespace pyc::cpp;

static void test_scalar_reg() {
  Wire<1> clk{0}, rst{0}, en{1};
  Wire<8> d{3}, init{0}, q{3};
  pyc_reg<8> reg(clk, rst, en, d, init, q);

  assert(!reg.tick_commit());
  reg.posedge_tick_compute();
  assert(!reg.tick_commit());
  reg.negedge_update();
  d = Wire<8>(4);
  reg.posedge_tick_compute();
  assert(reg.tick_commit());
}

static void test_vector_reg() {
  using V = Vec<Wire<8>, 2>;
  Wire<1> clk{0}, rst{0}, en{1};
  V d{{Wire<8>(1), Wire<8>(2)}};
  V init{};
  V q = d;
  pyc_vec_reg<V> reg(clk, rst, en, d, init, q);

  assert(!reg.tick_commit());
  reg.posedge_tick_compute();
  assert(!reg.tick_commit());
  reg.negedge_update();
  d[1] = Wire<8>(3);
  reg.posedge_tick_compute();
  assert(reg.tick_commit());
}

static void test_fifo() {
  Wire<1> clk{0}, rst{0}, inValid{0}, inReady{0}, outValid{0}, outReady{0};
  Wire<8> inData{0}, outData{0};
  pyc_fifo<8, 2> fifo(clk, rst, inValid, inReady, inData, outValid, outReady,
                      outData);

  assert(fifo.tick_commit() == 0);
  clk = Wire<1>(1);
  fifo.tick_compute();
  assert(fifo.tick_commit() == 0);
  clk = Wire<1>(0);
  fifo.tick_compute();
  inValid = Wire<1>(1);
  inData = Wire<8>(0x5a);
  clk = Wire<1>(1);
  fifo.tick_compute();
  const auto changed = fifo.tick_commit();
  assert((changed & pyc_fifo<8, 2>::kOutValidChanged) != 0);
  assert((changed & pyc_fifo<8, 2>::kOutDataChanged) != 0);
}

static void test_sync_mem() {
  Wire<1> clk{0}, rst{0}, ren{1}, wvalid{0};
  Wire<4> raddr{0}, waddr{0};
  Wire<8> rdata{0}, wdata{0};
  Wire<1> wstrb{1};
  pyc_sync_mem<4, 8, 4> mem(clk, rst, ren, raddr, rdata, wvalid, waddr, wdata,
                            wstrb);

  assert(!mem.tick_commit());
  clk = Wire<1>(1);
  mem.tick_compute();
  assert(!mem.tick_commit());
  clk = Wire<1>(0);
  mem.tick_compute();
  mem.pokeEntry(0, 7);
  clk = Wire<1>(1);
  mem.tick_compute();
  assert(mem.tick_commit());

  clk = Wire<1>(0);
  mem.tick_compute();
  ren = Wire<1>(0);
  wvalid = Wire<1>(1);
  wdata = Wire<8>(7);
  mem.mem_watch(0, 0);
  clk = Wire<1>(1);
  mem.tick_compute();
  assert(!mem.tick_commit());
  assert(mem.peekEntry(0) == 7);
  assert(mem.mem_watch_events().size() == 1);
  assert((mem.mem_watch_events()[0].kind ==
          pyc_sync_mem<4, 8, 4>::MemWatchEvent::Kind::Write));
}

static void test_sync_mem_dp() {
  Wire<1> clk{0}, rst{0}, ren0{1}, ren1{1}, wvalid{0};
  Wire<4> raddr0{0}, raddr1{1}, waddr{0};
  Wire<8> rdata0{0}, rdata1{0}, wdata{0};
  Wire<1> wstrb{1};
  pyc_sync_mem_dp<4, 8, 4> mem(clk, rst, ren0, raddr0, rdata0, ren1, raddr1,
                               rdata1, wvalid, waddr, wdata, wstrb);

  assert(mem.tick_commit() == 0);
  clk = Wire<1>(1);
  mem.tick_compute();
  assert(mem.tick_commit() == 0);
  clk = Wire<1>(0);
  mem.tick_compute();
  ren1 = Wire<1>(0);
  mem.pokeEntry(0, 9);
  clk = Wire<1>(1);
  mem.tick_compute();
  assert((mem.tick_commit() == pyc_sync_mem_dp<4, 8, 4>::kReadData0Changed));
}

static void test_byte_mem() {
  Wire<1> clk{0}, rst{0}, wvalid{1};
  Wire<4> raddr{0}, waddr{0};
  Wire<16> rdata{0}, wdata{0};
  Wire<2> wstrb{3};
  pyc_byte_mem<4, 16, 8> mem(clk, rst, raddr, rdata, wvalid, waddr, wdata,
                             wstrb);

  assert(!mem.tick_commit());
  clk = Wire<1>(1);
  mem.tick_compute();
  mem.mem_watch(0, 1);
  assert(!mem.tick_commit());
  assert(mem.peekByte(0) == 0);
  assert(!mem.mem_watch_events().empty());
  assert((mem.mem_watch_events()[0].kind ==
          pyc_byte_mem<4, 16, 8>::MemWatchEvent::Kind::Write));
  clk = Wire<1>(0);
  mem.tick_compute();
  wdata = Wire<16>(0x1234);
  clk = Wire<1>(1);
  mem.tick_compute();
  assert(mem.tick_commit());
}

static void test_async_fifo() {
  Wire<1> inClk{0}, inRst{0}, inValid{0}, inReady{0};
  Wire<8> inData{0};
  Wire<1> outClk{0}, outRst{0}, outValid{0}, outReady{0};
  Wire<8> outData{0};
  pyc_async_fifo<8, 2> fifo(inClk, inRst, inValid, inReady, inData, outClk,
                            outRst, outValid, outReady, outData);

  assert(fifo.tick_commit() == 0);
  inClk = Wire<1>(1);
  outClk = Wire<1>(1);
  fifo.tick_compute();
  assert(fifo.tick_commit() == 0);
  inClk = Wire<1>(0);
  outClk = Wire<1>(0);
  fifo.tick_compute();
  assert(fifo.tick_commit() == 0);

  inValid = Wire<1>(1);
  inData = Wire<8>(0x6b);
  inClk = Wire<1>(1);
  fifo.tick_compute();
  assert(fifo.tick_commit() == 0);
  inClk = Wire<1>(0);
  fifo.tick_compute();
  assert(fifo.tick_commit() == 0);

  pyc_async_fifo<8, 2>::change_mask_t changed = 0;
  for (unsigned i = 0; i < 3; ++i) {
    outClk = Wire<1>(1);
    fifo.tick_compute();
    changed = fifo.tick_commit();
    outClk = Wire<1>(0);
    fifo.tick_compute();
    assert(fifo.tick_commit() == 0);
  }
  assert((changed & pyc_async_fifo<8, 2>::kOutValidChanged) != 0);
  assert((changed & pyc_async_fifo<8, 2>::kOutDataChanged) != 0);
}

static void test_cdc_sync() {
  Wire<1> clk{0}, rst{0};
  Wire<8> in{0}, out{0};
  pyc_cdc_sync<8, 1> sync(clk, rst, in, out);

  assert(!sync.tick_commit());
  clk = Wire<1>(1);
  sync.tick_compute();
  assert(!sync.tick_commit());
  clk = Wire<1>(0);
  sync.tick_compute();
  in = Wire<8>(0x7c);
  clk = Wire<1>(1);
  sync.tick_compute();
  assert(sync.tick_commit());
}

int main() {
  test_scalar_reg();
  test_vector_reg();
  test_fifo();
  test_sync_mem();
  test_sync_mem_dp();
  test_byte_mem();
  test_async_fifo();
  test_cdc_sync();
  return 0;
}
