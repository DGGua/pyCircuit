#include "array_probe_slp.cpp"
int main() {
  pyc::gen::top sim;
  pyc::cpp::ProbeRegistry probes;
  sim.pyc_register_probes(probes, "dut");
  auto probe = [&](const char *name, unsigned value, unsigned width = 8) {
    const auto *entry = probes.findByPath(std::string("dut:") + name);
    if (!entry || entry->width_bits != width) return false;
    return width == 1 ? static_cast<const pyc::cpp::Wire<1> *>(entry->ptr)->value() == value
                      : static_cast<const pyc::cpp::Wire<8> *>(entry->ptr)->value() == value;
  };
  for (unsigned step = 0; step < 256; ++step) {
    unsigned a = step, b = (step * 7) & 255, c = (step * 31) & 255, d = (step * 11) & 255;
    bool s = step & 1, t = step & 2;
    sim.a = pyc::cpp::Wire<8>(a); sim.b = pyc::cpp::Wire<8>(b);
    sim.c = pyc::cpp::Wire<8>(c); sim.d = pyc::cpp::Wire<8>(d);
    sim.s = pyc::cpp::Wire<1>(s); sim.t = pyc::cpp::Wire<1>(t);
    sim.eval();
    if (!probe("xor0", a ^ b) || !probe("xor1", c ^ d) ||
        !probe("not0", (~a) & 255) || !probe("not1", (~c) & 255) ||
        !probe("mux0", s ? a : b) || !probe("mux1", t ? c : d) ||
        !probe("equal_pair[0]", a == b, 1) || !probe("equal_pair[1]", c == d, 1)) return 1;
    if (sim.xors[0].value() != (a ^ b) || sim.xors[1].value() != (c ^ d) ||
        sim.nots[0].value() != ((~a) & 255) || sim.nots[1].value() != ((~c) & 255) ||
        sim.muxes[0].value() != (s ? a : b) || sim.muxes[1].value() != (t ? c : d) ||
        sim.equals[0].value() != (a == b) || sim.equals[1].value() != (c == d)) return 2;
  }
}
