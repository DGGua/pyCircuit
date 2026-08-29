#include "supernode_state_boundary.hpp"

#include <cstdint>
#include <iostream>

#ifndef PYC_EXPECT_REG_UPDATE
#error "compile with -DPYC_EXPECT_REG_UPDATE=0(poll),1(commit)"
#endif

namespace {

using Dut = pyc::gen::supernode_state_boundary;

int fail(const char *message) {
  std::cerr << "supernode state-boundary gate: " << message << "\n";
  return 1;
}

void runPhase(Dut &dut) {
  dut.comb();
  dut.tick();
  dut.commit();
  dut.comb();
}

} // namespace

int main() {
  Dut dut;
  dut._pyc_sim_stats_enable = true;
  dut.reset_sim_stats();
  dut.clk = pyc::cpp::Wire<1>(0);
  dut.rst = pyc::cpp::Wire<1>(0);
  dut.enable = pyc::cpp::Wire<1>(0);

  dut.eval();
  const auto initialized = dut._pyc_sim_stats;
  if (initialized.comb_eval_calls == 0)
    return fail("first eval did not initialize the comb schedule");

  dut.eval();
  const auto idle = dut._pyc_sim_stats;
  if (idle.comb_eval_calls != initialized.comb_eval_calls)
    return fail("idle eval recomputed a comb partition");

  // Reset commits the current value (zero). It is a real pending commit but
  // must not wake register consumers because qNext == q.
  dut.rst = pyc::cpp::Wire<1>(1);
  dut.clk = pyc::cpp::Wire<1>(1);
  runPhase(dut);
  const auto sameValueReset = dut._pyc_sim_stats;
#if PYC_EXPECT_REG_UPDATE == 1
  if (sameValueReset.reg_commit_checks != 1)
    return fail("commit mode did not check the pending reset commit");
  if (sameValueReset.reg_semantic_changes != 0 ||
      sameValueReset.reg_fanout_enqueues != 0)
    return fail("same-value reset commit woke a register consumer");
#else
  if (sameValueReset.reg_commit_checks != 0 ||
      sameValueReset.reg_semantic_changes != 0 ||
      sameValueReset.reg_fanout_enqueues != 0)
    return fail("poll mode unexpectedly used commit-driven register stats");
#endif

  // Complete the falling edge, then commit an enabled increment on the next
  // rising edge. The post-commit comb must expose q=1 in the same step.
  dut.rst = pyc::cpp::Wire<1>(0);
  dut.clk = pyc::cpp::Wire<1>(0);
  runPhase(dut);
  dut.enable = pyc::cpp::Wire<1>(1);
  dut.clk = pyc::cpp::Wire<1>(1);
  const std::uint64_t evalsBeforeChange = dut._pyc_sim_stats.comb_eval_calls;
  runPhase(dut);
  const auto changed = dut._pyc_sim_stats;
  if (dut.current.word(0) != 1)
    return fail("post-commit comb did not publish the changed register value");
  if (changed.comb_eval_calls <= evalsBeforeChange)
    return fail("changed register did not execute its consumer comb");
#if PYC_EXPECT_REG_UPDATE == 1
  if (changed.reg_commit_checks != 2 ||
      changed.reg_semantic_changes != 1 ||
      changed.reg_fanout_enqueues != 1)
    return fail("commit mode register change counters are incorrect");
#endif

  std::cout << "ok reg-update=" << PYC_EXPECT_REG_UPDATE
            << " checks=" << changed.reg_commit_checks
            << " changes=" << changed.reg_semantic_changes
            << " fanout=" << changed.reg_fanout_enqueues << "\n";
  return 0;
}
