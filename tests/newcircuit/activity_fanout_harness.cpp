#include "activity.cpp"
#include <cstdlib>
#include <iostream>
#include <sstream>
#include <string>

int main() {
  pyc::gen::top sim;
  unsigned expected = 0;
  auto check = [&]() {
    sim.eval();
    for (unsigned i = 0; i < ACTIVITY_LANES; ++i)
      if (sim.out[i].value() != ((expected * (i + 2)) & 65535u)) std::abort();
  };
  for (unsigned cycle = 0; cycle < 256; ++cycle) {
    sim.clk = pyc::cpp::Wire<1>(0);
    sim.tick_compute();
    sim.tick_commit();
    bool reset = cycle % 19 == 0, enabled = cycle % 3 != 0;
    unsigned data = (cycle * 1997u) & 65535u;
    sim.rst = pyc::cpp::Wire<1>(reset);
    sim.en = pyc::cpp::Wire<1>(enabled);
    sim.d = pyc::cpp::Wire<16>(data);
    check();
    sim.clk = pyc::cpp::Wire<1>(1);
    sim.tick_compute();
    check();
    sim.tick_commit();
    expected = reset ? 19 : enabled ? data : expected;
    check();
    for (unsigned i = 0; i < 4; ++i) check();
  }
  std::ostringstream stats;
  sim.dump_sim_stats(stats);
  if (stats.str().find("group_cache_skips=0\n") != std::string::npos) return 1;
  std::cout << "packed fanout oracle passed: " << ACTIVITY_LANES << " lanes\n";
}
