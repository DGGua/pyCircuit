#include "array_probe_lanes.cpp"
#include <cstdio>

int main() {
  pyc::gen::top sim;
  pyc::cpp::ProbeRegistry probes;
  sim.pyc_register_probes(probes, "dut");
  auto probe = [&](const char *name, unsigned value) {
    const auto *entry = probes.findByPath(std::string("dut:") + name);
    return entry && entry->width_bits == 8 &&
           static_cast<const pyc::cpp::Wire<8> *>(entry->ptr)->value() == value;
  };
  const auto *state = probes.findByPath("dut:state_probe");
  if (!state || state->kind != pyc::cpp::ProbeKind::Reg ||
      !state->write_valid || !state->write_data_ptr) return 2;
  unsigned q[2] = {};
  for (unsigned cycle = 0; cycle < 192; ++cycle) {
    const bool reset = cycle == 0 || cycle % 19 == 0;
    const bool enable = cycle % 5 != 0;
    sim.rst = pyc::cpp::Wire<1>(reset);
    sim.en = pyc::cpp::Wire<1>(enable);
    for (unsigned row = 0; row < 2; ++row) {
      for (unsigned col = 0; col < 3; ++col) {
        sim.d[row][col] = pyc::cpp::Wire<8>((cycle * 31 + row * 47 + col * 13) & 255);
        sim.init[row][col] = pyc::cpp::Wire<8>((cycle * 7 + row * 43 + col * 29) & 255);
        sim.a[row][col] = pyc::cpp::Wire<8>((cycle * 17 + row * 71 + col * 11) & 255);
      }
    }
    sim.x = pyc::cpp::Wire<8>((cycle * 41) & 255);
    sim.z = pyc::cpp::Wire<8>((cycle * 59 + 3) & 255);
    sim.clk = pyc::cpp::Wire<1>(0); sim.step();
    const unsigned oldQ0 = q[0];
    // Register updates use an independent scalar model, including reset while
    // enable is low, wraparound, held state and changing reset values.
    for (unsigned lane = 0; lane < 2; ++lane) {
      unsigned row = lane, col = lane + 1;
      if (reset) q[lane] = (cycle * 7 + row * 43 + col * 29) & 255;
      else if (enable) q[lane] = (cycle * 31 + row * 47 + col * 13) & 255;
    }
    sim.clk = pyc::cpp::Wire<1>(1); sim.comb(); sim.tick();
    if (!probe("state_probe", oldQ0) || *state->write_valid != (reset || enable) ||
        ((reset || enable) &&
         static_cast<const pyc::cpp::Wire<8> *>(state->write_data_ptr)->value() != q[0]))
      return 3;
    sim.transfer(); sim.comb();
    const unsigned first = (q[0] + cycle * 17 + 11) & 255;
    const unsigned second = (q[1] + cycle * 17 + 71 + 22) & 255;
    if (sim.state01.value() != q[0] || !probe("state_probe", q[0]) ||
        sim.lane01.value() != first || sim.duplicate01.value() != first ||
        sim.lane12.value() != second ||
        !probe("sum_probe", first) || !probe("duplicate_probe", first) ||
        !probe("created_probe", (cycle * 59 + 3) & 255) ||
        !probe("broadcast_probe", (cycle * 41) & 255)) {
      std::fprintf(stderr, "array lane/probe mismatch at cycle %u\n", cycle);
      return 1;
    }
  }
}
