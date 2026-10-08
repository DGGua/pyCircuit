#include "cast_slice_demand.cpp"
#include <cstdint>

int main() {
  pyc::gen::top sim;
  std::uint32_t random = 0x9e3779b9u;
  for (unsigned trial = 0; trial < 256; ++trial) {
    random ^= random << 13;
    random ^= random >> 17;
    random ^= random << 5;
    unsigned a = random & 65535u;
    sim.a = pyc::cpp::Wire<16>(a);
    sim.eval();
    unsigned sign = (a & 0x8000u) ? 0xffu : 0u;
    if (sim.zlo.value() != ((a >> 4) & 255u) ||
        sim.zcross.value() != ((a >> 12) & 15u) ||
        sim.zhigh.value() != 0 ||
        sim.slo.value() != ((a >> 4) & 255u) ||
        sim.scross.value() != (((a >> 12) & 15u) | (sign & 0xf0u)) ||
        sim.shigh.value() != sign ||
        sim.tlo.value() != (a & 15u) ||
        sim.thi.value() != ((a >> 4) & 15u))
      return 1;
  }
  return 0;
}
