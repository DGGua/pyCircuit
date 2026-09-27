#include "scalar_demand.cpp"
#include <cstdint>
#include <iostream>

int main() {
  pyc::gen::top sim;
  const unsigned amounts[] = {0, 1, 3, 4, 7, 8, 15, 16, 31, 32, 63, 64, 127, 128, 129, 255};
  for (unsigned step = 0; step < 4096; ++step) {
    uint64_t lo = uint64_t(step) * 0x9e3779b97f4a7c15ULL ^ 0xffffffffffffffffULL;
    uint64_t hi = uint64_t(step) * 0xd6e8feb86659fd93ULL ^ 0x8000000000000000ULL;
    unsigned amount = amounts[step & 15];
    sim.a = pyc::cpp::Wire<128>({lo, hi});
    sim.amount = pyc::cpp::Wire<8>(amount);
    unsigned __int128 a = (static_cast<unsigned __int128>(hi) << 64) | lo;
    unsigned __int128 left = amount < 128 ? a << amount : 0;
    unsigned __int128 right = amount < 128 ? a >> amount : 0;
    auto wide = CASE_ID == 2 ? left : 0;
    auto expectedWide = pyc::cpp::Wire<128>({uint64_t(wide), uint64_t(wide >> 64)});
    sim.eval();
    if (sim.low.value() != (left & 15u) || sim.right.value() != (right & 15u) ||
        sim.middle.value() != (CASE_ID == 0 ? 0 : ((left >> 8) & 255u)) ||
        sim.wide != expectedWide) return 1;
  }
  std::cout << "dynamic left demand C++ oracle passed (4096 vectors)\n";
}
