#include <cstdint>
#include "multi_reader_immediate_shift.cpp"
int main() {
  pyc::gen::top sim;
  for (unsigned step = 0; step < 256; ++step) {
    std::uint64_t lo = UINT64_C(0x9e3779b97f4a7c15) * (step + 1);
    std::uint64_t hi = UINT64_C(0xd1b54a32d192ed03) * (step + 3);
    sim.a = pyc::cpp::Wire<128>({lo, hi});
    sim.eval();
    unsigned __int128 a = (static_cast<unsigned __int128>(hi) << 64) | lo;
    auto low = [](unsigned __int128 v) { return static_cast<unsigned>(v & 255); };
    auto mid = [](unsigned __int128 v) { return static_cast<unsigned>((v >> 32) & 255); };
    unsigned __int128 shiftedLeft = a << 7;
    unsigned __int128 shiftedRight = a >> 5;
    unsigned __int128 arithmetic = a >> 6;
    unsigned __int128 logicalFill = a >> 120;
    unsigned __int128 dynamicLeft = a << 3;
    unsigned __int128 dynamicRight = a >> 3;
    __int128 signedA = static_cast<__int128>(a);
    unsigned __int128 filled = static_cast<unsigned __int128>(signedA >> 120);
    unsigned __int128 signFill = (hi >> 63) ? ~static_cast<unsigned __int128>(0) : 0;
    if (sim.sh_low.value() != low(shiftedLeft) ||
        sim.sh_mid.value() != mid(shiftedLeft) ||
        sim.ls_low.value() != low(shiftedRight) ||
        sim.ls_mid.value() != mid(shiftedRight) ||
        sim.as_low.value() != low(arithmetic) ||
        sim.as_mid.value() != mid(arithmetic) ||
        sim.lsfill_low.value() != low(logicalFill) ||
        sim.lsfill_mid.value() != mid(logicalFill) ||
        sim.fill_low.value() != low(filled) ||
        sim.fill_mid.value() != mid(filled) ||
        sim.zero_low.value() != 0 || sim.zero_mid.value() != 0 ||
        sim.sign_low.value() != low(signFill) ||
        sim.sign_mid.value() != mid(signFill) ||
        sim.dyn_sh_low.value() != low(dynamicLeft) ||
        sim.dyn_sh_mid.value() != mid(dynamicLeft) ||
        sim.dyn_ls_low.value() != low(dynamicRight) ||
        sim.dyn_ls_mid.value() != mid(dynamicRight) ||
        sim.dyn_as_low.value() != low(dynamicRight) ||
        sim.dyn_as_mid.value() != mid(dynamicRight)) return 1;
  }
  return 0;
}
