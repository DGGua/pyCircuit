// A re-optimized intermediate file can already contain generated delay lines.
// Sharing them must never make the current pass's primitive counts negative.
//
// CHECK-LABEL: func.func @precombined_delay_lines
// CHECK-DAG: pyc.stats.delay_chain_delay_lines_created = 0 : i64
// CHECK-DAG: pyc.stats.delay_chain_delay_lines_merged = 1 : i64
// CHECK-DAG: pyc.stats.delay_chain_state_reads_after = 0 : i64
// CHECK-DAG: pyc.stats.delay_chain_state_reads_before = 0 : i64
// CHECK-DAG: pyc.stats.delay_chain_state_writes_after = 0 : i64
// CHECK-DAG: pyc.stats.delay_chain_state_writes_before = 0 : i64
// CHECK: pyc.delay_line
// CHECK-NOT: pyc.delay_line

module {
  func.func @precombined_delay_lines(
      %clk: !pyc.clock, %rst: !pyc.reset, %in: i8) -> (i8, i8) {
    %en = pyc.constant 1 : i1
    %init = pyc.constant 0 : i8
    %q0 = pyc.delay_line %clk, %rst, %en, %in, %init
        {depth = 2 : i64, pyc.generated = "cycle_balance"} : i8
    %q1 = pyc.delay_line %clk, %rst, %en, %in, %init
        {depth = 2 : i64, pyc.generated = "cycle_balance"} : i8
    func.return %q0, %q1 : i8, i8
  }
}

