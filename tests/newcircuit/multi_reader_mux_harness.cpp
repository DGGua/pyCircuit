#include <cstdint>
#include "multi_reader_mux.cpp"

int main() {
  pyc::gen::top sim;
  for (unsigned step = 0; step < 256; ++step) {
    std::uint64_t alo = UINT64_C(0x9e3779b97f4a7c15) * (step + 1);
    std::uint64_t ahi = UINT64_C(0xd1b54a32d192ed03) * (step + 3);
    std::uint64_t blo = UINT64_C(0x94d049bb133111eb) * (step + 7);
    std::uint64_t bhi = UINT64_C(0x2545f4914f6cdd1d) * (step + 11);
    sim.sel = pyc::cpp::Wire<1>(step & 1);
    sim.a = pyc::cpp::Wire<128>({alo, ahi});
    sim.b = pyc::cpp::Wire<128>({blo, bhi});
    sim.eval();
    std::uint64_t chosen = (step & 1) ? alo : blo;
    if (sim.low.value() != (chosen & 255) ||
        sim.mid.value() != ((chosen >> 32) & 255))
      return 1;
  }
  return 0;
}
