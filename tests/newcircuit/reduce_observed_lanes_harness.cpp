#include "reduce_observed_lanes.cpp"
#include <cstdint>

int main() {
  pyc::gen::top sim;
  std::uint32_t random = 0x4a94d717u;
  for (unsigned trial = 0; trial < 128; ++trial) {
    unsigned expectedOr0 = 0, expectedOr2 = 0, expectedAnd1 = 255;
    unsigned expectedSum2 = 0;
    for (unsigned row = 0; row < 3; ++row) {
      for (unsigned col = 0; col < 4; ++col) {
        random ^= random << 13;
        random ^= random >> 17;
        random ^= random << 5;
        unsigned value = random & 255u;
        sim.v[row][col] = pyc::cpp::Wire<8>(value);
        if (row == 0) expectedOr0 |= value;
        if (row == 2) expectedOr2 |= value;
        if (row == 1) expectedAnd1 &= value;
        if (col == 2) expectedSum2 = (expectedSum2 + value) & 255u;
      }
    }
    sim.eval();
    if (sim.or0.value() != expectedOr0 ||
        sim.or2.value() != expectedOr2 ||
        sim.and1.value() != expectedAnd1 ||
        sim.sum2.value() != expectedSum2)
      return 1;
  }
  return 0;
}
