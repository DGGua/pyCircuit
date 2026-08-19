#include "Vstate_delay_tap_codegen.h"

#include <cassert>
#include <cstdint>
#include <iostream>

int main() {
  Vstate_delay_tap_codegen model;
  std::uint8_t history[4] = {0, 0, 0, 0};
  std::uint64_t checksum = 0x9e3779b97f4a7c15ULL;
  constexpr unsigned kCycles = 257;
  for (unsigned cycle = 0; cycle < kCycles; ++cycle) {
    const bool reset = cycle == 0 || cycle == 1 || cycle == 113;
    const bool enable = cycle % 9 != 4 && cycle % 9 != 5;
    const std::uint8_t input = static_cast<std::uint8_t>(cycle * 37 + 11);
    model.rst = reset;
    model.en = enable;
    model.in = input;
    model.clk = 0;
    model.eval();
    model.clk = 1;
    model.eval();

    if (reset) {
      for (auto &value : history)
        value = 0;
    } else if (enable) {
      history[3] = history[2];
      history[2] = history[1];
      history[1] = history[0];
      history[0] = input;
    }
    assert(model.tap == history[1]);
    assert(model.tail == history[3]);
    checksum ^= model.tap + 0x9e3779b97f4a7c15ULL +
                (checksum << 6) + (checksum >> 2);
    checksum ^= model.tail + 0x517cc1b727220a95ULL +
                (checksum << 6) + (checksum >> 2);
  }
  std::cout << "cycles=" << kCycles << " checksum=" << checksum << "\n";
  return 0;
}
