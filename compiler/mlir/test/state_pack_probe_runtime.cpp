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
  if (!out0 || !out1 || out0->kind != pyc::cpp::ProbeKind::Reg ||
      out1->kind != pyc::cpp::ProbeKind::Reg || !lane0 || !lane1 ||
      lane0->kind != pyc::cpp::ProbeKind::Reg ||
      lane1->kind != pyc::cpp::ProbeKind::Reg)
    return 1;
  if (out0->width_bits != 8 || out1->width_bits != 8 ||
      out0->write_width_bits != 8 || out1->write_width_bits != 8 ||
      out0->write_storage_width_bits != 16 ||
      out1->write_storage_width_bits != 16 ||
      out0->write_lsb_bits != 0 || out1->write_lsb_bits != 8)
    return 2;
  if (out0->write_valid != out1->write_valid ||
      out0->write_data_ptr != out1->write_data_ptr)
    return 3;

  dut.clk = pyc::cpp::Wire<1>(0);
  dut.rst = pyc::cpp::Wire<1>(0);
  dut.en = pyc::cpp::Wire<1>(1);
  dut.a = pyc::cpp::Wire<8>(0xa5);
  dut.b = pyc::cpp::Wire<8>(0x3c);
  dut.step();
  dut.clk = pyc::cpp::Wire<1>(1);
  dut.comb();
  dut.tick();
  if (!*out0->write_valid)
    return 4;
  const auto *pending = static_cast<const pyc::cpp::Wire<16> *>(
      out0->write_data_ptr);
  if (!pending || pending->value() != UINT64_C(0x3ca5))
    return 5;
  return 0;
}
