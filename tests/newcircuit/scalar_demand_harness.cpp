#include "scalar_demand.cpp"
#include <cstdint>
#include <iostream>

int main() {
  pyc::gen::top sim;
  for (uint32_t step = 0; step < 4096; ++step) {
    uint32_t a = step * 0x9e3779b9u ^ 0x80000000u;
    uint32_t b = step * 0x85ebca6bu + 0xffffffffu;
    uint32_t c = step * 0xc2b2ae35u ^ 0xaaaaaaaa;
    uint32_t d = step * 0x27d4eb2fu + 0x55555555u;
    if ((step & 15) == 0) a = b = c = d = 0;
    if ((step & 15) == 1) a = b = c = d = 0xffffffffu;
    sim.a = pyc::cpp::Wire<32>(a);
    sim.b = pyc::cpp::Wire<32>(b);
    sim.c = pyc::cpp::Wire<32>(c);
    sim.d = pyc::cpp::Wire<32>(d);
    uint32_t y0 = 0, y1 = 0, y2 = 0, y3 = 0, wide = 0;
    uint32_t sum = a + b, inv = ~sum;
#if CASE_ID == 0
    y0 = inv & 15u;
#elif CASE_ID == 1
    uint32_t branch = (inv & c) ^ d;
    y0 = branch & 255u;
    y1 = (branch >> 4) & 4095u;
    y2 = ~(branch * a) & 255u;
    y3 = (sum - d) & 255u;
#elif CASE_ID == 2
    uint32_t bits = (a & b) & 255u;
    y0 = (bits + c) & 255u;
    y1 = (bits - d) & 255u;
    y2 = y3 = bits;
#elif CASE_ID == 3
    y0 = inv & 255u;
    y1 = (inv >> 8) & 4095u;
    wide = inv;
#else
    y0 = inv & 255u;
#endif
    sim.eval();
    if (sim.y0.value() != y0 || sim.y1.value() != y1 ||
        sim.y2.value() != y2 || sim.y3.value() != y3 ||
        sim.wide.value() != wide) {
      std::cerr << "scalar demand mismatch at " << step << '\n';
      return 1;
    }
  }
  std::cout << "scalar demand C++ oracle passed (4096 vectors)\n";
}
