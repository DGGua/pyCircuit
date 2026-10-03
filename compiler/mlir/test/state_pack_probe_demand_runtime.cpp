#ifndef MODEL_HEADER
#error "compile with -DMODEL_HEADER=generated_model.hpp"
#endif

#include MODEL_HEADER

#include <cstdint>

int main() {
  pyc::gen::state_pack_probe dut;
  pyc::cpp::ProbeRegistry probes;
  dut.pyc_register_probes(probes, "dut");

  const auto *out0 = probes.findByPath("dut:out0");
  const auto *out1 = probes.findByPath("dut:out1");
  const auto *lane0 = probes.findByPath("dut:lane0_state");
  const auto *lane1 = probes.findByPath("dut:lane1_state");
  if (!out0 || !out1 || !lane0 || !lane1)
    return 1;
  if (lane0->kind != pyc::cpp::ProbeKind::Reg ||
      lane1->kind != pyc::cpp::ProbeKind::Reg)
    return 2;
  if (lane0->width_bits != 8 || lane1->width_bits != 8)
    return 3;

  dut.clk = pyc::cpp::Wire<1>(0);
  dut.rst = pyc::cpp::Wire<1>(0);
  dut.en = pyc::cpp::Wire<1>(1);
  dut.a = pyc::cpp::Wire<8>(0xa5);
  dut.b = pyc::cpp::Wire<8>(0x3c);
  dut.step();
  dut.clk = pyc::cpp::Wire<1>(1);
  dut.step();

  if (lane0->readU64() != UINT64_C(0xa5) || lane1->readU64() != UINT64_C(0x3c))
    return 4;
  if (out0->readU64() != lane0->readU64() || out1->readU64() != lane1->readU64())
    return 5;
  return 0;
}
