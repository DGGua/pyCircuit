// STRIP-LABEL: func.func @drop_observation_only_state
// STRIP-SAME: pyc.stats.state_opt_observability_attrs_stripped = 4 : i64
// STRIP-SAME: pyc.stats.state_opt_observation_aliases_removed = 1 : i64
// STRIP-SAME: pyc.stats.state_opt_pinned_regs = 1 : i64
// STRIP-NOT: pyc.reg
// STRIP-NOT: pyc.alias
// STRIP: return

// STRIP-LABEL: func.func @keep_functional_state
// STRIP-SAME: pyc.stats.state_opt_observability_attrs_stripped = 2 : i64
// STRIP: %[[Q:.*]] = pyc.reg
// STRIP-NOT: pyc.debug_keep
// STRIP-NOT: pyc.name
// STRIP: return %[[Q]]

module {
  func.func @drop_observation_only_state(
      %clk: !pyc.clock, %rst: !pyc.reset, %in: i8) {
    %en = pyc.constant 1 : i1
    %init = pyc.constant 0 : i8
    %q = pyc.reg %clk, %rst, %en, %in, %init
        {pyc.debug_keep = true, pyc.observable = true} : i8
    %tap = pyc.alias %q
        {pyc.name = "debug_tap", pyc.probe.internal = true} : i8
    func.return
  }

  func.func @keep_functional_state(
      %clk: !pyc.clock, %rst: !pyc.reset, %in: i8) -> i8 {
    %en = pyc.constant 1 : i1
    %init = pyc.constant 0 : i8
    %q = pyc.reg %clk, %rst, %en, %in, %init
        {pyc.debug_keep = true, pyc.name = "functional_state"} : i8
    func.return %q : i8
  }
}
