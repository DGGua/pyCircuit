#ifdef GSIM_REFERENCE_NEWCIRCUIT
#include "gsim_reference.cpp"
struct ReferenceModel {
  pyc::gen::top model;
  void set_a(uint16_t value) { model.a = pyc::cpp::Wire<16>(value); }
  void set_b(uint16_t value) { model.b = pyc::cpp::Wire<16>(value); }
  void set_c(uint16_t value) { model.c = pyc::cpp::Wire<16>(value); }
  void set_sel(uint8_t value) { model.sel = pyc::cpp::Wire<1>(value); }
  void step() { model.eval(); }
  uint16_t get_sum16() { return model.sum16.value(); }
  uint16_t get_sub16() { return model.sub16.value(); }
  uint16_t get_product16() { return model.product16.value(); }
  uint16_t get_selected() { return model.selected.value(); }
  uint8_t get_low8() { return model.low8.value(); }
  uint8_t get_middle8() { return model.middle8.value(); }
  uint8_t get_crossing8() { return model.crossing8.value(); }
};
#else
#include "SmallReference.h"
using ReferenceModel = SSmallReference;
#endif
#include <array>
#include <cstdint>
#include <cstdio>
#include <new>

int main() {
  // Start with fully defined storage before GSIM's generated constructor.
  alignas(ReferenceModel) std::array<unsigned char, sizeof(ReferenceModel)> storage{};
  auto *sim = new (storage.data()) ReferenceModel;
  uint32_t random = 0x6d2b79f5u;
  uint64_t digest = 1469598103934665603ull;
  unsigned checked = 0;
  auto next = [&]() {
    random ^= random << 13; random ^= random >> 17; random ^= random << 5;
    return uint16_t(random);
  };
  auto check = [&](uint16_t a, uint16_t b, uint16_t c, bool select) {
    sim->set_a(a); sim->set_b(b); sim->set_c(c); sim->set_sel(select);
    sim->step();
    const uint16_t sum = uint16_t(uint32_t(a) + b);
    const uint16_t diff = uint16_t(uint32_t(a) - b);
    const uint32_t product = uint32_t(a) * b;
    const uint16_t selected = select ? sum : uint16_t(a ^ c);
    const uint8_t cross = uint8_t(((uint32_t(a) << 16) | b) >> 12);
    if (sim->get_sum16() != sum || sim->get_sub16() != diff ||
        sim->get_product16() != uint16_t(product) || sim->get_selected() != selected ||
        sim->get_low8() != uint8_t(selected) || sim->get_middle8() != uint8_t(product >> 8) ||
        sim->get_crossing8() != cross) {
      std::fprintf(stderr, "Reference oracle mismatch case=%u a=%u b=%u c=%u sel=%u\n", checked, a, b, c, select);
      return false;
    }
    for (uint64_t value : {uint64_t(sim->get_sum16()), uint64_t(sim->get_sub16()),
                          uint64_t(sim->get_product16()), uint64_t(sim->get_selected()),
                          uint64_t(sim->get_low8()), uint64_t(sim->get_middle8()),
                          uint64_t(sim->get_crossing8())})
      digest = (digest ^ value) * 1099511628211ull;
    ++checked;
    return true;
  };
  constexpr uint16_t edge[] = {0, 1, 0xff, 0x100, 0x7fff, 0x8000, 0xfffe, 0xffff};
  for (uint16_t a : edge)
    for (uint16_t b : edge)
      for (uint16_t c : edge)
        for (bool select : {false, true})
          if (!check(a, b, c, select)) return 1;
  for (unsigned i = 0; i < 65536; ++i) {
    uint16_t a = next(), b = next(), c = next();
    if (!check(a, b, c, i & 1) || !check(a, b, c, i & 1) ||
        !check(a, b, c, !(i & 1))) return 2;
  }
  std::printf("Arithmetic/mux/lowbits oracle PASS: %u checks; digest=%016llx\n",
              checked, static_cast<unsigned long long>(digest));
  sim->~ReferenceModel();
}
