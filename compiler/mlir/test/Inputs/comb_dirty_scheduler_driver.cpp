#include "comb_dirty_scheduler.cpp"

#include <cstdint>
#include <iostream>

#ifndef PYC_EXPECT_COMB_MODE
#error "compile with PYC_EXPECT_COMB_MODE=0(always),1(guarded),2(dirty)"
#endif

namespace {

using Dut = pyc::gen::comb_dirty_scheduler;

int fail(const char *message) {
  std::cerr << "comb dirty scheduler gate: " << message << "\n";
  return 1;
}

bool lowEq(const pyc::cpp::Wire<8> &value, std::uint64_t expected) {
  return value.word(0) == expected;
}

} // namespace

int main() {
  Dut dut;
  dut._pyc_sim_stats_enable = true;
  dut.a = pyc::cpp::Wire<8>(0x12);
  dut.mask = pyc::cpp::Wire<8>(0x00);
  dut.b = pyc::cpp::Wire<8>(0x03);
  dut.c = pyc::cpp::Wire<8>(0x05);

  dut.eval();
  const auto first = dut._pyc_sim_stats;
  if (first.comb_eval_calls != 5 || !lowEq(dut.producer, 0) ||
      !lowEq(dut.result, 6) || !lowEq(dut.constant_out, 0x5a))
    return fail("first evaluation did not initialize every comb group");

  dut.eval();
  const auto idle = dut._pyc_sim_stats;
#if PYC_EXPECT_COMB_MODE == 0
  if (idle.comb_eval_calls != first.comb_eval_calls + 5 ||
      idle.comb_guard_checks != 0)
    return fail("always mode was not the unconditional topology reference");
#else
  if (idle.comb_eval_calls != first.comb_eval_calls ||
      idle.comb_cache_skips < first.comb_cache_skips + 5)
    return fail("idle evaluation did not skip inactive comb groups");
#endif

  // The producer runs, but its semantic output remains zero. No downstream
  // branch or reconvergent consumer may run in guarded/dirty mode.
  dut.a = pyc::cpp::Wire<8>(0x34);
  dut.eval();
  const auto sameOutput = dut._pyc_sim_stats;
#if PYC_EXPECT_COMB_MODE != 0
  if (sameOutput.comb_eval_calls != idle.comb_eval_calls + 1 ||
      sameOutput.comb_output_semantic_changes !=
          idle.comb_output_semantic_changes)
    return fail("unchanged producer output woke downstream comb groups");
#endif

  dut.mask = pyc::cpp::Wire<8>(0xff);
  dut.eval();
  const auto changed = dut._pyc_sim_stats;
  if (!lowEq(dut.producer, 0x34) || !lowEq(dut.result, 0x0e))
    return fail("fanout/reconvergence produced the wrong value");
#if PYC_EXPECT_COMB_MODE != 0
  if (changed.comb_eval_calls < sameOutput.comb_eval_calls + 4)
    return fail("semantic change did not reach both fanouts and reconvergence");
#endif
#if PYC_EXPECT_COMB_MODE == 2
  if (changed.comb_fanout_enqueues < sameOutput.comb_fanout_enqueues + 3)
    return fail("dirty fanout did not activate exact direct consumers");
#endif

  std::cout << "ok mode=" << PYC_EXPECT_COMB_MODE
            << " evals=" << changed.comb_eval_calls
            << " skips=" << changed.comb_cache_skips
            << " fanout=" << changed.comb_fanout_enqueues << "\n";
  return 0;
}
