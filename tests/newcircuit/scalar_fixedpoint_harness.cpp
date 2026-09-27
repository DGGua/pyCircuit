#include "scalar_fixedpoint.cpp"
#include <cstdint>
#include <iostream>

int main() {
  pyc::gen::top sim;
  uint64_t q = 0;
  auto check = [&](uint64_t a, uint64_t b, bool sel, unsigned step) {
    uint64_t expected = a + b;
    if (CASE_MODEL == 1) expected = sel ? expected : a;
    if (CASE_MODEL == 2) expected <<= 3;
    if (CASE_MODEL == 3) expected = sel ? expected + a : expected * b;
    if (STATE_CASE) expected = q;
    sim.eval();
    if (sim.y.value() != (expected & 255u) ||
        sim.wide.value() != (FULL_RESULT ? expected : 0)) {
      std::cerr << "scalar fixedpoint mismatch at " << step << '\n';
      return false;
    }
    return true;
  };
  for (unsigned step = 0; step < 4096; ++step) {
    uint64_t a = step * UINT64_C(0x9e3779b97f4a7c15) ^ UINT64_C(0x8000000000000000);
    uint64_t b = step * UINT64_C(0xd6e8feb86659fd93) + UINT64_MAX;
    if ((step & 15) == 0) a = b = 0;
    if ((step & 15) == 1) a = b = UINT64_MAX;
    const bool reset = step % 31 == 0, enable = step % 4 != 0, sel = step % 3 != 0;
    sim.clk = pyc::cpp::Wire<1>(0);
    sim.tick_compute();
    sim.tick_commit();
    sim.a = pyc::cpp::Wire<64>(a);
    sim.b = pyc::cpp::Wire<64>(b);
    sim.rst = pyc::cpp::Wire<1>(reset);
    sim.en = pyc::cpp::Wire<1>(enable);
    sim.sel = pyc::cpp::Wire<1>(sel);
    if (!check(a, b, sel, step)) return 1;
    sim.clk = pyc::cpp::Wire<1>(1);
    sim.tick_compute();
    if (!check(a, b, sel, step)) return 1; // Compute cannot publish next state.
    sim.tick_commit();
    q = reset ? b : enable ? a + b : q;
    if (!check(a, b, sel, step)) return 1;
    sim.tick_commit();
    if (!check(a, b, sel, step)) return 1;
    if (STATE_CASE) {
      sim.a = pyc::cpp::Wire<64>(~a);
      sim.rst = pyc::cpp::Wire<1>(!reset);
      sim.tick_compute();
      sim.tick_commit();
      if (!check(~a, b, sel, step)) return 1; // No new rising edge.
    }
  }
  std::cout << "scalar fixedpoint C++ oracle passed (4096 cycles)\n";
}
