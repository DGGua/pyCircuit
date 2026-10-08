#include "pyc_primitives.hpp"
#include "pyc_vec.hpp"

#include <cassert>
#include <type_traits>

using pyc::cpp::Wire;
using pyc::cpp::pyc_delay_line;
using pyc::cpp::pyc_vec_delay_line;
using pyc::cpp::Vec;

static_assert(std::is_same_v<decltype(std::declval<pyc_delay_line<8, 2>&>().tick_commit()), bool>);
static_assert(std::is_same_v<
    decltype(std::declval<pyc_vec_delay_line<Vec<Wire<8>, 2>, 2>&>().tick_commit()), bool>);

namespace {

void posedge(pyc_delay_line<8, 2> &delay, Wire<1> &clk) {
  clk = Wire<1>(0);
  delay.tick_compute();
  clk = Wire<1>(1);
  delay.tick_compute();
}

void testScalarCommitReportsAdvance() {
  Wire<1> clk{}, rst{}, en{};
  Wire<8> d{}, init{}, q{};
  pyc_delay_line<8, 2> delay(clk, rst, en, d, init, q);

  // No pending edge -> no advance, so no wake needed.
  assert(!delay.tick_commit());

  rst = Wire<1>(1);
  en = Wire<1>(0);
  init = Wire<8>(0);
  posedge(delay, clk);
  // A reset commit advances state and must wake tap consumers even if the
  // tail happens to match init.
  assert(delay.tick_commit());
  assert(q.value() == 0);

  rst = Wire<1>(0);
  en = Wire<1>(1);
  d = Wire<8>(7);
  posedge(delay, clk);
  // Tail q is still 0, but tap(1) shifted from 0 to 7 - consumers must wake.
  assert(delay.tick_commit());
  assert(q.value() == 0);
  assert(delay.tap(1).value() == 7);

  d = Wire<8>(9);
  posedge(delay, clk);
  assert(delay.tick_commit());
  assert(q.value() == 7);

  d = Wire<8>(11);
  posedge(delay, clk);
  assert(delay.tick_commit());
  assert(q.value() == 9);
}

} // namespace

int main() {
  testScalarCommitReportsAdvance();
  return 0;
}
