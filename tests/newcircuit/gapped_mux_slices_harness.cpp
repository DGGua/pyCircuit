#include "gapped_mux_slices.cpp"
#include <cstdint>

int main() {
  pyc::gen::top sim;
  std::uint32_t random = 0x5b6a91c3u;
  for (unsigned trial = 0; trial < 128; ++trial) {
    random ^= random << 13;
    random ^= random >> 17;
    random ^= random << 5;
    unsigned a = random & 0xffffffu;
    random ^= random << 13;
    random ^= random >> 17;
    random ^= random << 5;
    unsigned b = random & 0xffffffu;
    unsigned sel = trial & 1u;
    sim.sel = pyc::cpp::Wire<1>(sel);
    sim.a = pyc::cpp::Wire<24>(a);
    sim.b = pyc::cpp::Wire<24>(b);
    sim.eval();
    unsigned selected = sel ? a : b;
    if (sim.lo.value() != (selected & 255u) ||
        sim.overlap.value() != ((selected >> 4) & 255u) ||
        sim.hi.value() != ((selected >> 20) & 15u))
      return 1;
  }
  return 0;
}
