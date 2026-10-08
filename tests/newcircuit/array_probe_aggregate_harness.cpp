#include "array_probe_aggregate.cpp"
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
    sim.sel = pyc::cpp::Wire<1>(step & 1);
    sim.x = pyc::cpp::Wire<8>(step);
    sim.z = pyc::cpp::Wire<8>((step + 23) & 255);
    for (unsigned lane = 0; lane < 3; ++lane) {
      sim.a[lane] = pyc::cpp::Wire<8>((step * 7 + lane * 19) & 255);
      sim.b[lane] = pyc::cpp::Wire<8>((step * 31 + lane * 11) & 255);
    }
    sim.eval();
    for (unsigned lane = 0; lane < 3; ++lane) {
      const unsigned a = (step * 7 + lane * 19) & 255;
      const unsigned b = (step * 31 + lane * 11) & 255;
      const std::string index = "[" + std::to_string(lane) + "]";
      if (!probe("aggregate_add" + index, (a + b) & 255) ||
          !probe("aggregate_mux" + index, step & 1 ? a : b)) return 1;
    }
    if (!probe("aggregate_create[0]", step) ||
        !probe("aggregate_create[1]", (step + 23) & 255) ||
        sim.added.value() != ((step * 38 + 30) & 255) ||
        sim.selected.value() != ((step & 1 ? step * 7 + 19 : step * 31 + 11) & 255) ||
        sim.created.value() != ((step + 23) & 255) ||
        sim.broadcasted.value() != step) return 2;
  }
}
