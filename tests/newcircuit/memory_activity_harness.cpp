#include "activity.cpp"
#include <array>
#include <cstdlib>
#include <iostream>

int main() {
  pyc::gen::top sim;
  std::array<unsigned, 4> memory{};
  unsigned address = 0;
  auto check = [&]() {
    sim.eval();
    unsigned v = memory[address];
    if (sim.rdata.value() != (((v + v) & 255u) ^ v)) std::abort();
  };
  for (unsigned cycle = 0; cycle < 512; ++cycle) {
    sim.clk = pyc::cpp::Wire<1>(0);
    sim.tick_compute();
    sim.tick_commit();
    address = cycle % 4;
    unsigned writeAddress = (cycle / 7) % 4, data = (cycle * 47) & 255u;
    bool valid = cycle % 5 != 0, strobe = cycle % 3 != 0, reset = cycle % 29 == 0;
    sim.raddr = pyc::cpp::Wire<2>(address);
    sim.waddr = pyc::cpp::Wire<2>(writeAddress);
    sim.wdata = pyc::cpp::Wire<8>(data);
    sim.wvalid = pyc::cpp::Wire<1>(valid);
    sim.wstrb = pyc::cpp::Wire<1>(strobe);
    sim.rst = pyc::cpp::Wire<1>(reset);
    check();
    sim.clk = pyc::cpp::Wire<1>(1);
    sim.tick_compute();
    check(); // Includes same-address old-data reads before transfer.
    sim.tick_commit();
    if (!reset && valid && strobe) memory[writeAddress] = data;
    check();
    check();
  }
  std::cout << "memory activity oracle passed (512 cycles)\n";
}
