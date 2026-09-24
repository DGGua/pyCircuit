#include <cstdint>
#include "multiuse_inline_under_test.cpp"
int main() {
  pyc::gen::top sim;
  for (unsigned step = 0; step < 256; ++step) {
    std::uint16_t a = static_cast<std::uint16_t>(step * 257u + 3u);
    std::uint16_t b = static_cast<std::uint16_t>(step * 193u + 17u);
    std::uint16_t c = static_cast<std::uint16_t>(step * 71u + 29u);
    sim.a = pyc::cpp::Wire<16>(a);
    sim.b = pyc::cpp::Wire<16>(b);
    sim.c = pyc::cpp::Wire<16>(c);
    sim.eval();
    std::uint16_t neg = static_cast<std::uint16_t>(~a);
    if (sim.x.value() != static_cast<std::uint16_t>(neg + b) ||
        sim.y.value() != static_cast<std::uint16_t>(neg - c)) return 1;
  }
  return 0;
}
