#include "cpp/pyc_primitives.hpp"
#include "cpp/pyc_vec.hpp"

#include <iostream>

namespace {

int fail(const char *message) {
  std::cerr << "pyc_reg commit gate: " << message << "\n";
  return 1;
}

} // namespace

int main() {
  using pyc::cpp::Vec;
  using pyc::cpp::Wire;

  Wire<1> clk(0), rst(0), en(0);
  Wire<130> d{}, init{}, q{};
  pyc::cpp::pyc_reg<130> wide(clk, rst, en, d, init, q);
  if (wide.tick_commit())
    return fail("non-pending wide commit reported a change");

  d = Wire<130>({0x1234, 0x2, 0x1});
  en = Wire<1>(1);
  clk = Wire<1>(1);
  wide.tick_compute();
  if (!wide.tick_commit() || q != d)
    return fail("wide register change was not committed");

  clk = Wire<1>(0);
  wide.tick_compute();
  clk = Wire<1>(1);
  wide.tick_compute();
  if (wide.tick_commit())
    return fail("same-value wide commit reported a change");

  Wire<1> bitClk(0), bitRst(0), bitEn(1), bitD(1), bitInit(0), bitQ(0);
  pyc::cpp::pyc_reg<1> bit(bitClk, bitRst, bitEn, bitD, bitInit, bitQ);
  bitClk = Wire<1>(1);
  bit.tick_compute();
  if (!bit.tick_commit() || !bitQ.toBool())
    return fail("packed-i1-compatible register commit failed");

  using Vec4x8 = Vec<Wire<8>, 4>;
  Wire<1> vecClk(0), vecRst(0), vecEn(1);
  Vec4x8 vecD{}, vecInit{}, vecQ{};
  vecD[0] = Wire<8>(1);
  vecD[1] = Wire<8>(2);
  vecD[2] = Wire<8>(3);
  vecD[3] = Wire<8>(4);
  pyc::cpp::pyc_vec_reg<Vec4x8> vec(vecClk, vecRst, vecEn, vecD, vecInit,
                                    vecQ);
  vecClk = Wire<1>(1);
  vec.tick_compute();
  if (!vec.tick_commit() || vecQ != vecD)
    return fail("vector register change was not committed");

  vecClk = Wire<1>(0);
  vec.tick_compute();
  vecClk = Wire<1>(1);
  vec.tick_compute();
  if (vec.tick_commit())
    return fail("same-value vector commit reported a change");

  std::cout << "ok: scalar, wide, packed-i1, and vector reg commits\n";
  return 0;
}
