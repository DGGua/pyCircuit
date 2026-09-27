#include "replication_under_test.cpp"
#include <cstdint>
#include <iostream>

int main() {
  pyc::gen::top sim;
  uint64_t random = 0x6a09e667f3bcc909ULL;
  uint64_t checksum = 0;
  unsigned state = 0;
  for (unsigned step = 0; step < 4096; ++step) {
    random ^= random << 13;
    random ^= random >> 7;
    random ^= random << 17;
    unsigned a = random & 255u, b = (random >> 8) & 255u;
    unsigned c = (random >> 16) & 255u, d = (random >> 24) & 255u;
    // Exercise carry boundaries, unchanged input, and changes outside demand.
    if ((step & 15u) < 4) a = b = c = d = (step & 1u) ? 255 : 0;
#if CASE_ID == 4
    uint64_t lo = random, hi = random * 0x9e3779b97f4a7c15ULL;
    sim.a = pyc::cpp::Wire<128>({lo, hi});
    unsigned mid = ((lo >> 60) | (hi << 4)) & 255u;
#elif CASE_ID == 5
    sim.v[0] = pyc::cpp::Wire<8>(a);
    sim.v[1] = pyc::cpp::Wire<8>(b);
    sim.c = pyc::cpp::Wire<8>(c);
    unsigned mid = (b + c) & 255u;
#elif CASE_ID == 6
    sim.v[0][0] = pyc::cpp::Wire<8>(a);
    sim.v[0][1] = pyc::cpp::Wire<8>(b);
    sim.v[1][0] = pyc::cpp::Wire<8>(c);
    sim.v[1][1] = pyc::cpp::Wire<8>(d);
    unsigned mid = d;
#elif CASE_ID == 7
    uint64_t hi = random * 0x9e3779b97f4a7c15ULL;
    sim.a = pyc::cpp::Wire<128>({random, hi});
    sim.b = pyc::cpp::Wire<128>({hi, random});
    sim.d = pyc::cpp::Wire<128>({random >> 1, hi >> 1});
    sim.eval();
    auto expected = pyc::cpp::Wire<128>({random ^ hi ^ ~(random >> 1),
                                        hi ^ random ^ ~(hi >> 1)});
    if (sim.y != expected) return 1;
    continue;
#elif CASE_ID == 8
    sim.a = pyc::cpp::Wire<8>(a);
    sim.c = pyc::cpp::Wire<8>(c);
    sim.d = pyc::cpp::Wire<8>(d);
    sim.next = pyc::cpp::Wire<8>(b);
    sim.init = pyc::cpp::Wire<8>(0x5a);
    bool reset = step % 31 == 0;
    bool enable = step % 7 != 0;
    sim.rst = pyc::cpp::Wire<1>(reset);
    sim.en = pyc::cpp::Wire<1>(enable);
    sim.clk = pyc::cpp::Wire<1>(0);
    sim.tick_compute();
    sim.tick_commit();
    sim.eval();
    if (sim.q.value() != state) return 2;
    sim.clk = pyc::cpp::Wire<1>(1);
    sim.tick_compute();
    if (sim.q.value() != state) return 3;
    sim.tick_commit();
    state = reset ? 0x5a : enable ? b : state;
    sim.eval();
    if (sim.q.value() != state) return 4;
    unsigned mid = ((state ^ a) + c) & 255u;
#else
    sim.a = pyc::cpp::Wire<8>(a);
    sim.b = pyc::cpp::Wire<8>(b);
    sim.c = pyc::cpp::Wire<8>(c);
#if CASE_ID == 9
    sim.sel = pyc::cpp::Wire<1>(step & 1u);
    unsigned mid = (((step & 1u) ? a : b) + c) & 255u;
#elif CASE_ID == 10
    // Keep the arithmetic oracle independent of division-by-zero policy.
    if (!b) b = 1;
    sim.b = pyc::cpp::Wire<8>(b);
    unsigned mid = ((a / b) + c) & 255u;
#else
    unsigned mid = ((a ^ b) + (CASE_ID == 1 ? (a ^ b) : c)) & 255u;
#endif
#endif
#if CASE_ID != 7
    sim.d = pyc::cpp::Wire<8>(d);
    sim.eval();
    unsigned expected = mid ^ ((~d) & 255u);
#if CASE_ID == 0 || CASE_ID == 1 || CASE_ID == 2 || CASE_ID == 5 || CASE_ID == 8 || CASE_ID >= 9
    expected = (expected + d) & 255u;
#endif
    if (sim.y.value() != expected) return 5;
#if CASE_ID == 3
    if (sim.z.value() != ((mid - ((~d) & 255u)) & 255u)) return 6;
#endif
    checksum = checksum * 131u + sim.y.value();
    sim.eval();
    if (sim.y.value() != expected) return 7;
#endif
  }
  std::cout << checksum << '\n';
  return 0;
}
