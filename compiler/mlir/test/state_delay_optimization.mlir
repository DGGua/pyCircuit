// Stage 1 regression fixture for provenance-independent state optimization.
//
// GENERATED-LABEL: func.func @merge_untagged
// GENERATED: %[[M0:.*]] = pyc.reg
// GENERATED: %[[M1:.*]] = pyc.reg
// GENERATED: return %[[M0]], %[[M1]]
// GENERATED-LABEL: func.func @form_untagged_chain
// GENERATED: pyc.reg
// GENERATED: pyc.reg
// GENERATED: pyc.reg
// GENERATED-NOT: pyc.delay_line
// GENERATED-LABEL: func.func @form_generated_chain
// GENERATED: pyc.delay_line
// GENERATED-SAME: depth = 2 : i64
// GENERATED-NOT: pyc.reg
// GENERATED-LABEL: func.func @merge_aliased_controls
// GENERATED-COUNT-2: pyc.reg
//
// STRUCTURAL-LABEL: func.func @merge_untagged
// STRUCTURAL-SAME: pyc.stats.state_opt_reg_bits_removed = 8 : i64
// STRUCTURAL-SAME: pyc.stats.state_opt_regs_merged = 1 : i64
// STRUCTURAL: %[[MERGED:.*]] = pyc.reg
// STRUCTURAL-NOT: pyc.reg
// STRUCTURAL: return %[[MERGED]], %[[MERGED]]
// STRUCTURAL-LABEL: func.func @merge_named_pair
// STRUCTURAL-SAME: pyc.stats.state_opt_regs_merged = 1 : i64
// STRUCTURAL: %[[NAMED:.*]] = pyc.reg
// STRUCTURAL-NOT: pyc.reg
// STRUCTURAL: pyc.name = "copy_a"
// STRUCTURAL: pyc.name = "copy_b"
// STRUCTURAL: return
// STRUCTURAL-LABEL: func.func @form_untagged_chain
// STRUCTURAL-SAME: pyc.stats.state_opt_structural_chain_regs_combined = 3 : i64
// STRUCTURAL-SAME: pyc.stats.state_opt_structural_chains_combined = 1 : i64
// STRUCTURAL: pyc.delay_line
// STRUCTURAL-SAME: depth = 3 : i64
// STRUCTURAL-SAME: pyc.optimized_by = "combine_delay_chains_structural"
// STRUCTURAL-NOT: pyc.reg
// STRUCTURAL-LABEL: func.func @keep_named_intermediate
// STRUCTURAL: pyc.delay_line
// STRUCTURAL-SAME: depth = 2 : i64
// STRUCTURAL: pyc.delay_tap
// STRUCTURAL: pyc.alias
// STRUCTURAL-SAME: pyc.name = "architectural_tap"
// STRUCTURAL-NOT: pyc.reg
// STRUCTURAL-LABEL: func.func @keep_fanout
// STRUCTURAL: pyc.delay_line
// STRUCTURAL: pyc.delay_tap
// STRUCTURAL-NOT: pyc.reg
// STRUCTURAL-LABEL: func.func @keep_stateful_fanout
// STRUCTURAL-COUNT-3: pyc.reg
// STRUCTURAL-NOT: pyc.delay_line
// STRUCTURAL-LABEL: func.func @keep_debug_state
// STRUCTURAL: pyc.delay_line
// STRUCTURAL-SAME: depth = 2 : i64
// STRUCTURAL: pyc.alias
// STRUCTURAL-SAME: pyc.debug_keep = true
// STRUCTURAL-NOT: pyc.reg
// STRUCTURAL-LABEL: func.func @keep_control_mismatch
// STRUCTURAL: pyc.reg
// STRUCTURAL: pyc.reg
// STRUCTURAL-NOT: pyc.delay_line
// STRUCTURAL-LABEL: func.func @form_generated_chain
// STRUCTURAL: pyc.delay_line
// STRUCTURAL-SAME: depth = 2 : i64

// AGGRESSIVE-LABEL: func.func @merge_untagged
// AGGRESSIVE-SAME: pyc.stats.state_opt_regs_merged = 1 : i64
// AGGRESSIVE: %[[MERGED:.*]] = pyc.reg
// AGGRESSIVE-NOT: pyc.reg
// AGGRESSIVE: return %[[MERGED]], %[[MERGED]]
// AGGRESSIVE-LABEL: func.func @merge_named_pair
// AGGRESSIVE: %[[NAMED:.*]] = pyc.reg
// AGGRESSIVE-NOT: pyc.reg
// AGGRESSIVE: pyc.name = "copy_a"
// AGGRESSIVE: pyc.name = "copy_b"
// AGGRESSIVE-LABEL: func.func @form_untagged_chain
// AGGRESSIVE: pyc.delay_line
// AGGRESSIVE-SAME: depth = 3 : i64
// AGGRESSIVE-NOT: pyc.reg
// AGGRESSIVE-LABEL: func.func @keep_named_intermediate
// AGGRESSIVE: pyc.delay_line
// AGGRESSIVE-SAME: depth = 2 : i64
// AGGRESSIVE: pyc.alias
// AGGRESSIVE-SAME: pyc.name = "architectural_tap"
// AGGRESSIVE-NOT: pyc.reg
// AGGRESSIVE-LABEL: func.func @keep_fanout
// AGGRESSIVE-SAME: pyc.stats.delay_chain_taps_created = 1 : i64
// AGGRESSIVE: pyc.delay_line
// AGGRESSIVE: pyc.delay_tap
// AGGRESSIVE-SAME: depth = 1 : i64
// AGGRESSIVE-NOT: pyc.reg
// AGGRESSIVE-LABEL: func.func @keep_stateful_fanout
// AGGRESSIVE-COUNT-3: pyc.reg
// AGGRESSIVE-NOT: pyc.delay_line
// AGGRESSIVE-NOT: pyc.delay_tap
// AGGRESSIVE-LABEL: func.func @keep_debug_state
// AGGRESSIVE: pyc.delay_line
// AGGRESSIVE-SAME: depth = 2 : i64
// AGGRESSIVE: pyc.alias
// AGGRESSIVE-SAME: pyc.debug_keep = true
// AGGRESSIVE-NOT: pyc.reg
// AGGRESSIVE-LABEL: func.func @keep_control_mismatch
// AGGRESSIVE: pyc.reg
// AGGRESSIVE: pyc.reg
// AGGRESSIVE-NOT: pyc.delay_line
// AGGRESSIVE-LABEL: func.func @form_generated_chain
// AGGRESSIVE: pyc.delay_line
// AGGRESSIVE-SAME: depth = 2 : i64
// AGGRESSIVE-LABEL: func.func @merge_aliased_controls
// AGGRESSIVE-SAME: pyc.stats.state_opt_regs_merged = 1 : i64
// AGGRESSIVE-COUNT-1: pyc.reg
// AGGRESSIVE: return %{{.*}}, %{{.*}}

module {
  func.func @merge_untagged(%clk: !pyc.clock, %rst: !pyc.reset, %in: i8) -> (i8, i8) {
    %en = pyc.constant 1 : i1
    %init = pyc.constant 0 : i8
    %q0 = pyc.reg %clk, %rst, %en, %in, %init : i8
    %q1 = pyc.reg %clk, %rst, %en, %in, %init : i8
    func.return %q0, %q1 : i8, i8
  }

  func.func @merge_named_pair(%clk: !pyc.clock, %rst: !pyc.reset, %in: i8) -> (i8, i8) {
    %en = pyc.constant 1 : i1
    %init = pyc.constant 0 : i8
    %q0 = pyc.reg %clk, %rst, %en, %in, %init : i8
    %a0 = pyc.alias %q0 {pyc.name = "copy_a"} : i8
    %q1 = pyc.reg %clk, %rst, %en, %in, %init : i8
    %a1 = pyc.alias %q1 {pyc.name = "copy_b"} : i8
    func.return %a0, %a1 : i8, i8
  }

  func.func @form_untagged_chain(%clk: !pyc.clock, %rst: !pyc.reset, %in: i8) -> i8 {
    %en = pyc.constant 1 : i1
    %init = pyc.constant 0 : i8
    %q0 = pyc.reg %clk, %rst, %en, %in, %init : i8
    %q1 = pyc.reg %clk, %rst, %en, %q0, %init : i8
    %q2 = pyc.reg %clk, %rst, %en, %q1, %init : i8
    func.return %q2 : i8
  }

  func.func @keep_named_intermediate(%clk: !pyc.clock, %rst: !pyc.reset, %in: i8) -> i8 {
    %en = pyc.constant 1 : i1
    %init = pyc.constant 0 : i8
    %q0 = pyc.reg %clk, %rst, %en, %in, %init : i8
    %tap = pyc.alias %q0 {pyc.name = "architectural_tap"} : i8
    %q1 = pyc.reg %clk, %rst, %en, %tap, %init : i8
    func.return %q1 : i8
  }

  func.func @keep_fanout(%clk: !pyc.clock, %rst: !pyc.reset, %in: i8) -> (i8, i8) {
    %en = pyc.constant 1 : i1
    %init = pyc.constant 0 : i8
    %q0 = pyc.reg %clk, %rst, %en, %in, %init : i8
    %q1 = pyc.reg %clk, %rst, %en, %q0, %init : i8
    func.return %q0, %q1 : i8, i8
  }

  func.func @keep_stateful_fanout(
      %clk: !pyc.clock, %rst: !pyc.reset, %in: i8, %en2: i1) -> (i8, i8) {
    %en = pyc.constant 1 : i1
    %init = pyc.constant 0 : i8
    %q0 = pyc.reg %clk, %rst, %en, %in, %init : i8
    %q1 = pyc.reg %clk, %rst, %en, %q0, %init : i8
    %q2 = pyc.reg %clk, %rst, %en2, %q0, %init : i8
    func.return %q1, %q2 : i8, i8
  }

  func.func @keep_debug_state(%clk: !pyc.clock, %rst: !pyc.reset, %in: i8) -> i8 {
    %en = pyc.constant 1 : i1
    %init = pyc.constant 0 : i8
    %q0 = pyc.reg %clk, %rst, %en, %in, %init {pyc.debug_keep = true} : i8
    %q1 = pyc.reg %clk, %rst, %en, %q0, %init : i8
    func.return %q1 : i8
  }

  func.func @keep_control_mismatch(%clk: !pyc.clock, %rst: !pyc.reset,
                                   %in: i8, %en0: i1, %en1: i1) -> i8 {
    %init = pyc.constant 0 : i8
    %q0 = pyc.reg %clk, %rst, %en0, %in, %init : i8
    %q1 = pyc.reg %clk, %rst, %en1, %q0, %init : i8
    func.return %q1 : i8
  }

  func.func @form_generated_chain(%clk: !pyc.clock, %rst: !pyc.reset, %in: i8) -> i8 {
    %en = pyc.constant 1 : i1
    %init = pyc.constant 0 : i8
    %q0 = pyc.reg %clk, %rst, %en, %in, %init {pyc.generated = "cycle_balance"} : i8
    %q1 = pyc.reg %clk, %rst, %en, %q0, %init {pyc.generated = "cycle_balance"} : i8
    func.return %q1 : i8
  }

  func.func @merge_aliased_controls(
      %clk: !pyc.clock, %rst: !pyc.reset, %in: i8) -> (i8, i8) {
    %clk_alias = pyc.alias %clk : !pyc.clock
    %rst_alias = pyc.alias %rst : !pyc.reset
    %en = pyc.constant 1 : i1
    %init = pyc.constant 0 : i8
    %q0 = pyc.reg %clk, %rst, %en, %in, %init : i8
    %q1 = pyc.reg %clk_alias, %rst_alias, %en, %in, %init : i8
    func.return %q0, %q1 : i8, i8
  }
}
