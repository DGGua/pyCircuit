#include "supernode_optional_update.hpp"

#include <cstdint>
#include <iostream>

#ifndef PYC_EXPECT_COMB_MODE
#error "compile with -DPYC_EXPECT_COMB_MODE=0(always),1(guarded),2(dirty)"
#endif

#ifndef PYC_EXPECT_GROUPED
#define PYC_EXPECT_GROUPED 0
#endif

namespace {

using Dut = pyc::gen::supernode_optional_update;

int fail(const char *message) {
  std::cerr << "supernode optional-update gate: " << message << "\n";
  return 1;
}

template <unsigned Width>
bool lowEq(const pyc::cpp::Wire<Width> &value, std::uint64_t expected) {
  return value.word(0) == expected;
}

void setCommonInputs(Dut &dut) {
  dut.a = pyc::cpp::Wire<8>(0x12);
  dut.mask = pyc::cpp::Wire<8>(0x00);
  dut.b = pyc::cpp::Wire<8>(0x03);
  dut.c = pyc::cpp::Wire<8>(0x05);

  dut.wide = pyc::cpp::Wire<130>({0x1234, 0x2, 0x1});
  dut.wide_mask = pyc::cpp::Wire<130>({0x0, 0x0, 0x0});
  dut.wide_bias = pyc::cpp::Wire<130>({0x55, 0x1, 0x0});

  dut.vec_a0 = pyc::cpp::Wire<8>(0x10);
  dut.vec_a1 = pyc::cpp::Wire<8>(0x20);
  dut.vec_a2 = pyc::cpp::Wire<8>(0x30);
  dut.vec_a3 = pyc::cpp::Wire<8>(0x40);
  dut.vec_mask0 = pyc::cpp::Wire<8>(0x00);
  dut.vec_mask1 = pyc::cpp::Wire<8>(0x00);
  dut.vec_mask2 = pyc::cpp::Wire<8>(0x00);
  dut.vec_mask3 = pyc::cpp::Wire<8>(0x00);
  dut.vec_bias0 = pyc::cpp::Wire<8>(0x01);
  dut.vec_bias1 = pyc::cpp::Wire<8>(0x02);
  dut.vec_bias2 = pyc::cpp::Wire<8>(0x03);
  dut.vec_bias3 = pyc::cpp::Wire<8>(0x04);
}

} // namespace

int main() {
  Dut dut;
  dut._pyc_sim_stats_enable = true;
  dut.reset_sim_stats();
  setCommonInputs(dut);

  // First evaluation must initialize every SuperNode, including the
  // zero-input constant partition.
  dut.eval();
  const auto first = dut._pyc_sim_stats;
  if (first.comb_eval_calls == 0)
    return fail("first eval did not execute any comb partition");
  if (!lowEq(dut.producer, 0x00) || !lowEq(dut.result, 0x00) ||
      !lowEq(dut.wide_out, 0x55) || !lowEq(dut.vec_out0, 0x01) ||
      !lowEq(dut.vec_out1, 0x02) || !lowEq(dut.vec_out2, 0x03) ||
      !lowEq(dut.vec_out3, 0x04) || !lowEq(dut.constant_out, 0x5A) ||
      !lowEq(dut.reset_seen, 0x0))
    return fail(
        "first eval produced incorrect scalar/wide/vector/constant values");

  // Identical inputs: guarded/dirty must do no comb work and no output-store
  // attempt.  Always mode is the forced-recompute reference lane.
  dut.eval();
  const auto unchanged = dut._pyc_sim_stats;
#if PYC_EXPECT_COMB_MODE == 0
  if (unchanged.comb_eval_calls <= first.comb_eval_calls)
    return fail("always mode skipped an unchanged-input evaluation");
  if (unchanged.comb_output_store_attempts <= first.comb_output_store_attempts)
    return fail("always mode did not perform unconditional output stores");
  if (unchanged.comb_guard_checks != 0 || unchanged.comb_cache_skips != 0)
    return fail("always mode unexpectedly used optional-update guards");
#else
  if (unchanged.comb_eval_calls != first.comb_eval_calls)
    return fail("optional-update mode recomputed with identical inputs");
  if (unchanged.comb_output_store_attempts != first.comb_output_store_attempts)
    return fail("optional-update mode attempted a store with identical inputs");
  if (unchanged.comb_cache_skips <= first.comb_cache_skips)
    return fail("optional-update mode did not record unchanged-input skips");
#endif

  // Change a producer input while mask==0.  Exactly the producer should run in
  // guarded/dirty mode; its output remains zero, so both fanouts and the
  // reconvergent consumer must remain asleep.
  dut.a = pyc::cpp::Wire<8>(0x34);
  dut.eval();
  const auto maskedChange = dut._pyc_sim_stats;
  if (!lowEq(dut.producer, 0x00) || !lowEq(dut.result, 0x00))
    return fail("masked producer input change altered an output");
#if PYC_EXPECT_COMB_MODE == 0
  if (maskedChange.comb_eval_calls <= unchanged.comb_eval_calls)
    return fail(
        "always mode failed to force recomputation after an input change");
#else
  if (maskedChange.comb_eval_calls != unchanged.comb_eval_calls + 1)
    return fail("unchanged producer output woke downstream SuperNodes");
  if (maskedChange.comb_output_semantic_changes !=
      unchanged.comb_output_semantic_changes)
    return fail("masked producer was reported as a semantic output change");
#endif
#if PYC_EXPECT_COMB_MODE == 2
  if (maskedChange.comb_fanout_enqueues != unchanged.comb_fanout_enqueues)
    return fail(
        "dirty mode propagated activity for an unchanged producer output");
#endif

  // Make the producer output change.  Dirty mode must propagate activity down
  // both fanouts through reconvergence; guarded mode reaches the same semantic
  // result through snapshots.
  dut.mask = pyc::cpp::Wire<8>(0xFF);
  dut.eval();
  const auto propagated = dut._pyc_sim_stats;
  if (!lowEq(dut.producer, 0x34) || !lowEq(dut.result, 0x08))
    return fail("fanout/reconvergence produced the wrong result");
#if PYC_EXPECT_COMB_MODE != 0
  // max-nodes=1 exposes every operation as a runtime unit.  The grouped lane
  // uses max-nodes=3, where the producer/left branch share one multi-output
  // partition and the reconvergent right/result path shares another.
  constexpr std::uint64_t expectedFanoutWork = PYC_EXPECT_GROUPED ? 2 : 6;
  if (propagated.comb_eval_calls <
      maskedChange.comb_eval_calls + expectedFanoutWork)
    return fail("producer semantic change did not reach all fanout consumers");
  if (propagated.comb_output_semantic_changes <=
      maskedChange.comb_output_semantic_changes)
    return fail("producer semantic change was not conditionally published");
#endif
#if PYC_EXPECT_COMB_MODE == 2
  if (propagated.comb_fanout_enqueues <= maskedChange.comb_fanout_enqueues)
    return fail("dirty mode did not enqueue changed producer fanout");
#endif

  // Exercise vector-valued and >64-bit partition boundaries.
  dut.vec_mask0 = pyc::cpp::Wire<8>(0xFF);
  dut.vec_mask1 = pyc::cpp::Wire<8>(0xFF);
  dut.vec_mask2 = pyc::cpp::Wire<8>(0xFF);
  dut.vec_mask3 = pyc::cpp::Wire<8>(0xFF);
  dut.wide_mask = pyc::cpp::Wire<130>::ones();
  dut.eval();
  if (!lowEq(dut.vec_out0, 0x11) || !lowEq(dut.vec_out1, 0x22) ||
      !lowEq(dut.vec_out2, 0x33) || !lowEq(dut.vec_out3, 0x44))
    return fail("vector partition equality/publish path is incorrect");
  if (dut.wide_out.word(0) != (0x1234ull ^ 0x55ull) ||
      dut.wide_out.word(1) != (0x2ull ^ 0x1ull) ||
      dut.wide_out.word(2) != 0x1ull)
    return fail("wide partition equality/publish path is incorrect");

  // !pyc.reset is an equality-comparable boundary type and reset_active is on
  // the memoizable-op whitelist; this must not regress the default pipeline.
  dut.rst = pyc::cpp::Wire<1>(1);
  dut.eval();
  if (!lowEq(dut.reset_seen, 0x1))
    return fail("reset_active/!pyc.reset partition path is incorrect");

  std::cout << "ok mode=" << PYC_EXPECT_COMB_MODE
            << " evals=" << dut._pyc_sim_stats.comb_eval_calls
            << " skips=" << dut._pyc_sim_stats.comb_cache_skips
            << " changes=" << dut._pyc_sim_stats.comb_output_semantic_changes
            << " fanout=" << dut._pyc_sim_stats.comb_fanout_enqueues << "\n";
  return 0;
}
