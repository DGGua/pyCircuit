#include "cost_inline_under_test.cpp"

int main() {
  pyc::gen::top sim;
  for (unsigned value = 0; value < 256; ++value) {
    unsigned a = value, b = (value * 17u) & 255u;
    unsigned c = (value * 29u) & 255u, d = (value * 13u) & 255u;
    unsigned e = (value * 7u) & 255u;
    sim.a = pyc::cpp::Wire<8>(a);
    sim.b = pyc::cpp::Wire<8>(b);
    sim.c = pyc::cpp::Wire<8>(c);
    sim.d = pyc::cpp::Wire<8>(d);
    sim.e = pyc::cpp::Wire<8>(e);
    sim.eval();
    if (sim.y.value() != (((((a + b) & 255u) ^ c) & d) + e) % 256u)
      return 1;
  }
  return 0;
}
