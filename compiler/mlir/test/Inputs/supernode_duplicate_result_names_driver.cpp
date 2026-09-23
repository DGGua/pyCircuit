#include "supernode_duplicate_result_names.hpp"

#include <cpp/pyc_probe_registry.hpp>

int main() {
  pyc::gen::supernode_duplicate_result_names dut;
  pyc::cpp::ProbeRegistry registry;
  dut.pyc_register_probes(registry, "dut");
  const auto *tapA = registry.findByPath("dut:tap_a");
  const auto *tapB = registry.findByPath("dut:tap_b");
  if (!tapA || !tapB || tapA->width_bits != 8 || tapB->width_bits != 8 ||
      tapA->ptr == nullptr || tapB->ptr == nullptr)
    return 1;
  return 0;
}
