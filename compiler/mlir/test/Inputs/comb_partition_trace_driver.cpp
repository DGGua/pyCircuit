#include "comb_dirty_scheduler.cpp"

#include <cstdint>
#include <iostream>

// Trace driver: sweeps input combinations and prints the DUT outputs each
// cycle. Used to diff default vs partitioned comb emission bit-for-bit.
namespace {
using Dut = pyc::gen::comb_dirty_scheduler;
} // namespace

int main() {
  Dut dut;
  std::uint64_t state = 0x9e3779b97f4a7c15ull;
  auto next = [&state]() {
    state ^= state << 13;
    state ^= state >> 7;
    state ^= state << 17;
    return state;
  };

  for (int cycle = 0; cycle < 256; ++cycle) {
    if (cycle % 3 == 0) {
      dut.a = pyc::cpp::Wire<8>(static_cast<std::uint8_t>(next()));
      dut.mask = pyc::cpp::Wire<8>(static_cast<std::uint8_t>(next()));
    }
    if (cycle % 5 == 0)
      dut.b = pyc::cpp::Wire<8>(static_cast<std::uint8_t>(next()));
    if (cycle % 7 == 0)
      dut.c = pyc::cpp::Wire<8>(static_cast<std::uint8_t>(next()));
    dut.eval();
    std::cout << cycle << ' ' << dut.producer.word(0) << ' '
              << dut.result.word(0) << ' ' << dut.constant_out.word(0) << "\n";
  }
  return 0;
}
