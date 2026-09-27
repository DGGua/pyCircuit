#include "array_probe_roots.cpp"
int main() {
  pyc::gen::top sim;
  pyc::cpp::ProbeRegistry probes;
  sim.pyc_register_probes(probes, "dut");
  auto probe = [&](const char *name, unsigned value) {
    const auto *entry = probes.findByPath(std::string("dut:") + name);
    return entry && entry->width_bits == 8 &&
           static_cast<const pyc::cpp::Wire<8> *>(entry->ptr)->value() == value;
  };
  for (const char *name : {"unused_name_hint", "unused_state_hint", "disabled_keep"})
    if (probes.findByPath(std::string("dut:") + name)) return 1;
  const auto *state = probes.findByPath("dut:kept_state");
  if (!state || state->kind != pyc::cpp::ProbeKind::Reg ||
      !state->write_valid || !state->write_data_ptr) return 2;
  auto flag = [&](const char *name, unsigned value) {
    const auto *entry = probes.findByPath(std::string("dut:") + name);
    return entry && entry->width_bits == 1 && entry->kind == pyc::cpp::ProbeKind::Reg &&
           static_cast<const pyc::cpp::Wire<1> *>(entry->ptr)->value() == value;
  };
  unsigned q = 0, qb = 0, q0 = 0, q1 = 0;
  for (unsigned step = 0; step < 256; ++step) {
    const unsigned a = (step * 17 + 3) & 255, b = (step * 29 + 5) & 255;
    const bool reset = step % 19 == 0, enable = step % 5 != 0;
    sim.a = pyc::cpp::Wire<8>(a); sim.b = pyc::cpp::Wire<8>(b);
    sim.rst = pyc::cpp::Wire<1>(reset); sim.en = pyc::cpp::Wire<1>(enable);
    sim.clk = pyc::cpp::Wire<1>(0); sim.step();
    sim.clk = pyc::cpp::Wire<1>(1); sim.comb(); sim.tick();
    if (!probe("kept_sum", (a + b) & 255) ||
        !probe("kept_duplicate", (a + b) & 255) ||
        !probe("kept_identity", a) || !probe("kept_wire", a) ||
        !probe("nested_keep", a ^ b) ||
        !probe("kept_state", q) || !probe("kept_vector[0]", q) ||
        !probe("kept_vector[1]", qb) || *state->write_valid != (reset || enable)) return 3;
    if (reset) q = 0; else if (enable) q = a;
    if (reset) qb = 0; else if (enable) qb = b;
    if (reset) q0 = q1 = 0;
    else if (enable) { q0 = a & 1; q1 = b & 1; }
    if ((reset || enable) &&
        static_cast<const pyc::cpp::Wire<8> *>(state->write_data_ptr)->value() != q) return 4;
    sim.transfer(); sim.comb();
    if (sim.y.value() != b || !probe("kept_state", q) ||
        !probe("kept_vector[0]", q) || !probe("kept_vector[1]", qb) ||
        !flag("kept_flag0", q0) || !flag("kept_flag1", q1) ||
        !flag("kept_alias_flag0", q0) || !flag("kept_alias_flag1", q1)) return 5;
  }
}
