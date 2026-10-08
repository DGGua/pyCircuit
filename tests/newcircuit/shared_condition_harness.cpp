#include <cstdint>
#include GENERATED_CIRCUIT

static uint8_t first(unsigned kind, uint8_t a, uint8_t b) {
  switch (kind) {
  case 0: return a + b;
  case 1: return a ^ b;
  case 2: return a | b;
  case 3: return a & b;
  case 4: return a - b;
  default: return a * b;
  }
}

int main() {
  pyc::gen::top sim;
  sim.guard = pyc::cpp::Wire<1>(1);
  for (unsigned a = 0; a < 256; a += 3) {
    for (unsigned b = 0; b < 256; b += 17) {
      const uint8_t c = a * 31 + b * 7;
      for (unsigned sel : {0u, 1u, 1u, 0u}) {
        sim.sel = pyc::cpp::Wire<1>(sel);
        sim.a = pyc::cpp::Wire<8>(a);
        sim.b = pyc::cpp::Wire<8>(b);
        sim.c = pyc::cpp::Wire<8>(c);
        sim.eval();
        uint8_t expected[6];
        for (unsigned i = 0; i < 6; ++i) {
          uint8_t left = a;
#if defined(EFFECT_BARRIER)
          left = a + b;
#elif defined(DEPENDENT_MUX)
          if (i) left = expected[i - 1];
#endif
#if defined(DEPENDENT_MUX)
          uint8_t value = left + b;
#else
          uint8_t value = first(i, left, b);
#endif
#if defined(INLINE_TREE)
          const unsigned second[] = {1, 0, 4, 5, 2, 3};
          value = first(second[i], value, c);
#endif
          expected[i] = sel ? value : c;
        }
        if (sim.out0.value() != expected[0] || sim.out1.value() != expected[1] ||
            sim.out2.value() != expected[2] || sim.out3.value() != expected[3] ||
            sim.out4.value() != expected[4] || sim.out5.value() != expected[5])
          return 1;
      }
    }
  }
  return 0;
}
