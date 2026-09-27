#include "activity.cpp"
#include <cstdlib>
#include <iostream>
#include <sstream>
#include <string>

static unsigned long long counter(pyc::gen::top &sim, const char *name) {
  std::ostringstream out;
  sim.dump_sim_stats(out);
  const std::string key = std::string(name) + "=";
  auto pos = out.str().find(key);
  if (pos == std::string::npos) std::abort();
  return std::stoull(out.str().substr(pos + key.size()));
}

int main() {
  pyc::gen::top sim;
  unsigned q0 = 0, q1 = 0;
  auto check = [&]() {
    sim.eval();
    if (sim.q0.value() != q0 || sim.q1.value() != q1 ||
        sim.y.value() != (((q0 + q1) & 255u) ^ q0)) std::abort();
  };
  check();
  const auto first = counter(sim, "group_eval_calls");
  check();
  if (std::getenv("EXPECT_ACTIVITY") &&
      (first == 0 || counter(sim, "group_eval_calls") != first ||
       counter(sim, "group_cache_skips") == 0)) return 1;
  for (unsigned cycle = 0; cycle < 512; ++cycle) {
    unsigned d0 = (cycle * 71u) & 255u, d1 = (cycle * 103u) & 255u;
    bool reset = cycle % 31 == 0, en0 = cycle % 4 != 0, en1 = cycle % 7 != 0;
    sim.clk = pyc::cpp::Wire<1>(0);
    sim.tick_compute();
    sim.tick_commit();
    sim.rst = pyc::cpp::Wire<1>(reset);
    sim.en0 = pyc::cpp::Wire<1>(en0);
    sim.en1 = pyc::cpp::Wire<1>(en1);
    sim.d0 = pyc::cpp::Wire<8>(d0);
    sim.d1 = pyc::cpp::Wire<8>(d1);
    check();
    sim.clk = pyc::cpp::Wire<1>(1);
    sim.tick_compute();
    check(); // TICK-OBS still exposes current state.
    sim.tick_commit();
    q0 = reset ? 17 : en0 ? d0 : q0;
    q1 = reset ? 34 : en1 ? d1 : q1;
    check();
    // Repeated commit and a high clock cannot manufacture another edge.
    sim.tick_commit();
    check();
    sim.d0 = pyc::cpp::Wire<8>(d0 ^ 255u);
    sim.rst = pyc::cpp::Wire<1>(!reset);
    sim.tick_compute();
    sim.tick_commit();
    check();
    const auto stable = counter(sim, "group_eval_calls");
    for (unsigned i = 0; i < 3; ++i) check();
    if (std::getenv("EXPECT_ACTIVITY") &&
        counter(sim, "group_eval_calls") != stable) return 2;
  }
  std::cout << "state activity oracle passed (512 cycles)\n";
}
