#include "primitive_activity_checks.h"
#include <array>

int main() {
  pyc::gen::top sim;
  std::array<unsigned, 4> memory{};
  unsigned q0 = 0, q1 = 0;
  auto check = [&]() {
    sim.eval();
    require(sim.q0.value() == q0 && sim.q1.value() == q1, "dual-port memory read oracle mismatch");
    require(sim.y.value() == (((q0 + q1) & 255u) ^ q0), "dual-port memory stale consumer");
  };
  for (unsigned cycle = 0; cycle < 1024; ++cycle) {
    sim.clk = pyc::cpp::Wire<1>(0);
    sim.tick_compute();
    sim.tick_commit();
    bool reset = cycle % 67 == 0;
    bool en0 = cycle % 5 != 0, en1 = cycle % 7 != 0;
    unsigned addr0 = cycle & 7u, addr1 = (cycle / 3u) & 7u;
    unsigned waddr = cycle % 3 == 0 ? addr0 : (cycle / 5u) & 7u;
    unsigned data = (cycle * 79u + 23u) & 255u;
    bool write = cycle % 4 != 0, strobe = cycle % 6 != 0;
    sim.rst = pyc::cpp::Wire<1>(reset);
    sim.ren0 = pyc::cpp::Wire<1>(en0);
    sim.ren1 = pyc::cpp::Wire<1>(en1);
    sim.raddr0 = pyc::cpp::Wire<3>(addr0);
    sim.raddr1 = pyc::cpp::Wire<3>(addr1);
    sim.wvalid = pyc::cpp::Wire<1>(write);
    sim.waddr = pyc::cpp::Wire<3>(waddr);
    sim.wdata = pyc::cpp::Wire<8>(data);
    sim.wstrb = pyc::cpp::Wire<1>(strobe);
    check();
    sim.clk = pyc::cpp::Wire<1>(1);
    sim.tick_compute();
    check();
    sim.tick_commit();
    if (reset) q0 = q1 = 0;
    else {
      // Both ports read old data before this cycle's masked write commits.
      if (en0) q0 = addr0 < memory.size() ? memory[addr0] : 0;
      if (en1) q1 = addr1 < memory.size() ? memory[addr1] : 0;
      if (write && strobe && waddr < memory.size()) memory[waddr] = data;
    }
    checkStable(sim, check);
    sim.tick_compute(); // Holding the clock high cannot capture another read.
    sim.tick_commit();
    check();
  }
  std::cout << "dual-port memory activity oracle passed (1024 cycles)\n";
}
