#include "array_probe_reductions.cpp"
int main() {
  pyc::gen::top sim;
  pyc::cpp::ProbeRegistry probes;
  sim.pyc_register_probes(probes, "dut");
  auto probe = [&](const std::string &name, unsigned value) {
    const auto *entry = probes.findByPath("dut:" + name);
    return entry && entry->width_bits == 8 &&
           static_cast<const pyc::cpp::Wire<8> *>(entry->ptr)->value() == value;
  };
  for (unsigned step = 0; step < 256; ++step) {
    unsigned expectedOr = 0, expectedAnd = 255, expectedSum = 0;
    for (unsigned row = 0; row < 3; ++row)
      for (unsigned col = 0; col < 4; ++col) {
        unsigned value = (step * 37 + row * 73 + col * 29) & 255;
        sim.v[row][col] = pyc::cpp::Wire<8>(value);
        if (row == 0) expectedOr |= value;
        if (row == 1) expectedAnd &= value;
        if (col == 2) expectedSum += value;
      }
    for (unsigned col = 0; col < 4; ++col)
      sim.r[col] = pyc::cpp::Wire<8>((step * 17 + col * 11) & 255);
    sim.eval();
    expectedSum &= 255;
    unsigned expectedLane = (step * 17 + 22) & 255;
    if (sim.or0.value() != expectedOr || sim.and1.value() != expectedAnd ||
        sim.sum2.value() != expectedSum || sim.row2.value() != expectedLane ||
        sim.col1.value() != expectedLane ||
        !probe("or_probe", expectedOr) || !probe("and_probe", expectedAnd) ||
        !probe("sum_probe", expectedSum)) return 1;
    for (unsigned lane = 0; lane < 4; ++lane)
      if (!probe("row_probe[" + std::to_string(lane) + "]",
                 (step * 17 + lane * 11) & 255)) return 2;
    for (unsigned lane = 0; lane < 2; ++lane)
      if (!probe("column_probe[" + std::to_string(lane) + "]", expectedLane)) return 3;
  }
}
