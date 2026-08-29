#include "supernode_named_probe.hpp"

#include <cpp/pyc_probe_registry.hpp>

int main() {
  pyc::gen::supernode_named_probe dut;
  pyc::cpp::ProbeRegistry registry;
  dut.pyc_register_probes(registry, "dut");
  const auto *tap = registry.findByPath("dut:tap");
  if (!tap || tap->kind != pyc::cpp::ProbeKind::Wire ||
      tap->width_bits != 8 || tap->ptr == nullptr)
    return 1;
  return 0;
}
