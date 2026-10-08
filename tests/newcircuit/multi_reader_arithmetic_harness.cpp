#include <cstdint>
#include "multi_reader_arithmetic.cpp"
int main() {
  pyc::gen::top sim;
  for (unsigned step = 0; step < 256; ++step) {
    std::uint64_t alo = UINT64_C(0x9e3779b97f4a7c15) * (step + 1);
    std::uint64_t ahi = UINT64_C(0xd1b54a32d192ed03) * (step + 3);
    std::uint64_t blo = UINT64_C(0x94d049bb133111eb) * (step + 7);
    std::uint64_t bhi = UINT64_C(0x2545f4914f6cdd1d) * (step + 11);
    sim.a = pyc::cpp::Wire<128>({alo, ahi});
    sim.b = pyc::cpp::Wire<128>({blo, bhi});
    sim.eval();
    unsigned __int128 a = (static_cast<unsigned __int128>(ahi) << 64) | alo;
    unsigned __int128 b = (static_cast<unsigned __int128>(bhi) << 64) | blo;
    auto low = [](unsigned __int128 value) {
      return static_cast<unsigned>((value >> 0) & 255);
    };
    auto middle = [](unsigned __int128 value) {
      return static_cast<unsigned>((value >> 32) & 255);
    };
    if (sim.add_low.value() != low(a + b) ||
        sim.add_mid.value() != middle(a + b) ||
        sim.sub_low.value() != low(a - b) ||
        sim.sub_mid.value() != middle(a - b) ||
        sim.mul_low.value() != low(a * b) ||
        sim.mul_mid.value() != middle(a * b)) return 1;
  }
  return 0;
}
