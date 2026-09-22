#include "comb_dirty_state.cpp"

#include <cstdint>
#include <iostream>

namespace {

using Dut = pyc::gen::comb_dirty_state;

int fail(const char *message) {
  std::cerr << "comb dirty state gate: " << message << "\n";
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
  dut.clk = pyc::cpp::Wire<1>(0);
  dut.rst = pyc::cpp::Wire<1>(0);
  dut.enable = pyc::cpp::Wire<1>(0);

  dut.eval();
  const auto initialized = dut._pyc_sim_stats.comb_eval_calls;
  dut.eval();
  if (dut._pyc_sim_stats.comb_eval_calls != initialized)
    return fail("idle state design recomputed a comb group");

  dut.clk = pyc::cpp::Wire<1>(1);
  runPhase(dut);
  dut.clk = pyc::cpp::Wire<1>(0);
  runPhase(dut);
  dut.enable = pyc::cpp::Wire<1>(1);
  dut.clk = pyc::cpp::Wire<1>(1);
  const std::uint64_t before = dut._pyc_sim_stats.comb_eval_calls;
  runPhase(dut);
  if (dut.current.word(0) != 1)
    return fail("post-commit comb did not expose the new register value");
  if (dut._pyc_sim_stats.comb_eval_calls <= before)
    return fail("changed register did not wake its boundary consumer");

  dut.clk = pyc::cpp::Wire<1>(0);
  runPhase(dut);
  dut.enable = pyc::cpp::Wire<1>(0);
  dut.clk = pyc::cpp::Wire<1>(1);
  dut.comb();
  dut.tick();
  const std::uint64_t beforeSameCommit =
      dut._pyc_sim_stats.comb_eval_calls;
  const std::uint64_t commitChanges =
      dut._pyc_sim_stats.commit_changes;
  dut.commit();
  dut.comb();
  if (dut._pyc_sim_stats.comb_eval_calls != beforeSameCommit)
    return fail("same-value register commit woke a comb consumer");
  if (dut._pyc_sim_stats.commit_changes != commitChanges)
    return fail("same-value register commit was counted as a change");

  dut.clk = pyc::cpp::Wire<1>(0);
  runPhase(dut);
  dut.rst = pyc::cpp::Wire<1>(1);
  dut.clk = pyc::cpp::Wire<1>(1);
  const std::uint64_t resetChanges = dut._pyc_sim_stats.commit_changes;
  runPhase(dut);
  if (dut.current.word(0) != 0)
    return fail("reset did not restore the visible register output");
  if (dut._pyc_sim_stats.commit_changes != resetChanges + 1)
    return fail("reset output transition was not reported exactly once");
  if (dut._pyc_sim_stats.source_checks == 0 ||
      dut._pyc_sim_stats.source_changes == 0)
    return fail("state-dirty source statistics were not recorded");

  std::cout << "ok post-commit current=" << dut.current.word(0) << "\n";
  return 0;
}
