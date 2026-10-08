#include <cstdint>
#include GENERATED_CIRCUIT

int main() {
  pyc::gen::top sim;
  for (unsigned a = 0; a < 256; ++a) {
    for (unsigned b = 0; b < 256; b += 17) {
      const unsigned c = (a * 37 + b * 11) & 255;
      sim.a = pyc::cpp::Wire<8>(a);
      sim.b = pyc::cpp::Wire<8>(b);
      sim.c = pyc::cpp::Wire<8>(c);
      sim.eval();
#ifdef COST_DAG
      const uint8_t n0 = a + b, n1 = a ^ c, n2 = b | c;
      const uint8_t n3 = n1 + n2, n4 = n0 ^ n2;
      const uint8_t n5 = n0 | n4, n6 = n4 + n5, n7 = n0 ^ n3;
      if (sim.out0.value() != n0 || sim.out1.value() != n1 ||
          sim.out2.value() != n2 || sim.out3.value() != n3 ||
          sim.out4.value() != n4 || sim.out5.value() != n5 ||
          sim.out6.value() != n6 || sim.out7.value() != n7)
        return 1;
#else
      uint8_t expected = a + b;
      expected ^= c;
      expected += a;
      expected ^= b;
      expected += c;
      expected ^= a;
      expected += b;
      expected ^= c;
      if (sim.out.value() != expected)
        return 1;
#endif
      // Repeated unchanged evaluation must preserve the same output.
      sim.eval();
    }
  }
  return 0;
}
