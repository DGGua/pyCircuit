#include "primitive_activity_checks.h"
#include <array>

int main() {
  pyc::gen::top sim;
  std::array<unsigned, 3> history{};
  auto check = [&]() {
    sim.eval();
    unsigned q = history.back();
    require(sim.q.value() == q, "CDC stage-latency oracle mismatch");
    require(sim.y.value() == (((q + q) & 255u) ^ q), "CDC stale consumer");
  };
  for (unsigned cycle = 0; cycle < 1024; ++cycle) {
    sim.clk = pyc::cpp::Wire<1>(0);
    sim.tick_compute();
    sim.tick_commit();
    bool reset = cycle % 59 == 0;
    unsigned data = (cycle * 41u) & 255u;
    sim.rst = pyc::cpp::Wire<1>(reset);
    sim.d = pyc::cpp::Wire<8>(data);
    check();
    sim.clk = pyc::cpp::Wire<1>(1);
    sim.tick_compute();
    check();
    sim.tick_commit();
    if (reset) history.fill(0);
    else history = {data, history[0], history[1]};
    checkStable(sim, check);
    sim.d = pyc::cpp::Wire<8>(data ^ 255u);
    sim.tick_compute();
    sim.tick_commit();
    check();
  }
  std::cout << "CDC activity oracle passed (1024 cycles)\n";
}
