// A pass-positive chain and a fanout-negative chain. This file is intended for
// pyc-opt/FileCheck once the pass is registered, and can also be inspected via
// pycc --dump-pass-ir.
module {
  func.func @combine_generated_chain(%clk: !pyc.clock, %rst: !pyc.reset, %in: i8) -> i8 {
    %en0 = pyc.constant 1 : i1
    %init0 = pyc.constant 0 : i8
    %q0 = pyc.reg %clk, %rst, %en0, %in, %init0 {pyc.generated = "cycle_balance"} : i8
    %a0 = pyc.alias %q0 {pyc.generated = "cycle_balance", pyc.name = "_v5_bal_1"} : i8
    %en1 = pyc.constant 1 : i1
    %init1 = pyc.constant 0 : i8
    %q1 = pyc.reg %clk, %rst, %en1, %a0, %init1 {pyc.generated = "cycle_balance"} : i8
    %a1 = pyc.alias %q1 {pyc.generated = "cycle_balance", pyc.name = "_v5_bal_2"} : i8
    %en2 = pyc.constant 1 : i1
    %init2 = pyc.constant 0 : i8
    %q2 = pyc.reg %clk, %rst, %en2, %a1, %init2 {pyc.generated = "cycle_balance"} : i8
    func.return %q2 : i8
  }

  func.func @keep_intermediate_fanout(%clk: !pyc.clock, %rst: !pyc.reset, %in: i8) -> (i8, i8) {
    %en0 = pyc.constant 1 : i1
    %init0 = pyc.constant 0 : i8
    %q0 = pyc.reg %clk, %rst, %en0, %in, %init0 {pyc.generated = "cycle_balance"} : i8
    %en1 = pyc.constant 1 : i1
    %init1 = pyc.constant 0 : i8
    %q1 = pyc.reg %clk, %rst, %en1, %q0, %init1 {pyc.generated = "cycle_balance"} : i8
    func.return %q0, %q1 : i8, i8
  }

  func.func @combine_generated_vector_chain(%clk: !pyc.clock, %rst: !pyc.reset,
                                            %in: vector<2xi8>, %init: vector<2xi8>) -> vector<2xi8> {
    %en = pyc.constant 1 : i1
    %q0 = pyc.reg %clk, %rst, %en, %in, %init {pyc.generated = "cycle_balance"} : vector<2xi8>
    %q1 = pyc.reg %clk, %rst, %en, %q0, %init {pyc.generated = "cycle_balance"} : vector<2xi8>
    func.return %q1 : vector<2xi8>
  }

  func.func @keep_untagged_chain(%clk: !pyc.clock, %rst: !pyc.reset, %in: i8) -> i8 {
    %en = pyc.constant 1 : i1
    %init = pyc.constant 0 : i8
    %q0 = pyc.reg %clk, %rst, %en, %in, %init : i8
    %q1 = pyc.reg %clk, %rst, %en, %q0, %init : i8
    func.return %q1 : i8
  }

  func.func @keep_debug_state(%clk: !pyc.clock, %rst: !pyc.reset, %in: i8) -> i8 {
    %en = pyc.constant 1 : i1
    %init = pyc.constant 0 : i8
    %q0 = pyc.reg %clk, %rst, %en, %in, %init {pyc.debug_keep = true, pyc.generated = "cycle_balance"} : i8
    %q1 = pyc.reg %clk, %rst, %en, %q0, %init {pyc.generated = "cycle_balance"} : i8
    func.return %q1 : i8
  }
}
