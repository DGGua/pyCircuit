#include "scalar_demand.cpp"
#include <cstdint>
#include <iostream>

int main() {
  pyc::gen::top sim;
  const uint64_t amounts[] = {0, 1, 31, 32, 63, 64, 127, 128, 255,
                             1ULL << 32, (1ULL << 32) + 1, 1ULL << 63,
                             UINT64_MAX, 0, 1, 127};
  for (unsigned step = 0; step < 4096; ++step) {
    uint64_t lo = uint64_t(step) * 0x9e3779b97f4a7c15ULL ^ UINT64_MAX;
    uint64_t hi = uint64_t(step) * 0xd6e8feb86659fd93ULL ^ (1ULL << 63);
    uint64_t amountLo = amounts[step & 15];
    uint64_t amountHi = AMOUNT_WIDTH == 128 && (step & 15) >= 13
                            ? (1ULL << ((step & 15) == 15 ? 63 : 0)) : 0;
    sim.a = pyc::cpp::Wire<128>({lo, hi});
#if AMOUNT_WIDTH == 128
    sim.amount = pyc::cpp::Wire<128>({amountLo, amountHi});
#else
    sim.amount = pyc::cpp::Wire<64>(amountLo);
#endif
    unsigned __int128 a = (static_cast<unsigned __int128>(hi) << 64) | lo;
    unsigned __int128 ones = ~static_cast<unsigned __int128>(0);
    bool overflow = amountHi != 0 || amountLo >= 128;
    unsigned __int128 left = overflow ? 0 : a << amountLo;
    unsigned __int128 logical = overflow ? 0 : a >> amountLo;
    unsigned __int128 arithmetic = logical;
    if (hi >> 63) {
      if (overflow) arithmetic = ones;
      else if (amountLo) arithmetic |= ones << (128 - amountLo);
    }
    auto wire = [](unsigned __int128 value) {
      return pyc::cpp::Wire<128>({uint64_t(value), uint64_t(value >> 64)});
    };
    sim.eval();
    if (sim.left != wire(left) || sim.logical != wire(logical) ||
        sim.arithmetic != wire(arithmetic) || sim.low.value() != (left & 15u)) {
      std::cerr << "wide shift amount mismatch at " << step << '\n';
      return 1;
    }
  }
  std::cout << "wide shift amount C++ oracle passed (4096 vectors)\n";
}
