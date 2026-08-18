// Regression fixture for delay-chain provenance and compile-stat reporting.
// Two identical generated depth-2 chains must fold to one shared delay line.
//
// CHECK-LABEL: func.func @delay_line_diagnostics
// CHECK-DAG: pyc.stats.delay_chain_aliases_removed = 2 : i64
// CHECK-DAG: pyc.stats.delay_chain_delay_lines_created = 2 : i64
// CHECK-DAG: pyc.stats.delay_chain_delay_lines_merged = 1 : i64
// CHECK-DAG: pyc.stats.delay_chain_regs_combined = 4 : i64
// CHECK-DAG: pyc.stats.delay_chain_state_reads_after = 1 : i64
// CHECK-DAG: pyc.stats.delay_chain_state_reads_before = 4 : i64
// CHECK-DAG: pyc.stats.delay_chain_state_writes_after = 1 : i64
// CHECK-DAG: pyc.stats.delay_chain_state_writes_before = 4 : i64
// CHECK-DAG: pyc.stats.delay_chains_combined = 2 : i64
// CHECK: pyc.delay_line
// CHECK-SAME: depth = 2 : i64
// CHECK-SAME: pyc.generated = "cycle_balance"
// CHECK-SAME: pyc.optimized_by = "combine_delay_chains"
// CHECK-SAME: pyc.shared_chain_count = 2 : i64
// CHECK-SAME: pyc.source_reg_count = 4 : i64
// CHECK-NOT: pyc.reg

module attributes {pyc.top = @delay_line_diagnostics, pyc.frontend.contract = "pycircuit"} {
  func.func @delay_line_diagnostics(
      %clk: !pyc.clock, %rst: !pyc.reset, %in: i8) -> (i8, i8)
      attributes {arg_names = ["clk", "rst", "in"], result_names = ["out0", "out1"],
                  pyc.kind = "module", pyc.inline = "false", pyc.params = "{}",
                  pyc.base = "delay_line_diagnostics",
                  pyc.struct.metrics = "{\22ast_node_count\22:0,\22collection_count\22:0,\22collection_instance_count\22:0,\22estimated_inline_cost\22:0,\22hardware_call_count\22:0,\22instance_count\22:0,\22loop_count\22:0,\22module_call_count\22:0,\22module_family_collection_count\22:0,\22repeat_pressure\22:0,\22repeated_body_clusters\22:[],\22source_loc\22:0,\22state_alloc_count\22:0,\22state_call_count\22:0}",
                  pyc.struct.collections = "[]", pyc.value_params = [],
                  pyc.value_param_types = []} {
    %en = pyc.constant 1 : i1
    %init = pyc.constant 0 : i8

    %q00 = pyc.reg %clk, %rst, %en, %in, %init
        {pyc.generated = "cycle_balance"} : i8
    %a0 = pyc.alias %q00
        {pyc.generated = "cycle_balance", pyc.name = "_diag_bal_0"} : i8
    %q01 = pyc.reg %clk, %rst, %en, %a0, %init
        {pyc.generated = "cycle_balance"} : i8

    %q10 = pyc.reg %clk, %rst, %en, %in, %init
        {pyc.generated = "cycle_balance"} : i8
    %a1 = pyc.alias %q10
        {pyc.generated = "cycle_balance", pyc.name = "_diag_bal_1"} : i8
    %q11 = pyc.reg %clk, %rst, %en, %a1, %init
        {pyc.generated = "cycle_balance"} : i8

    func.return %q01, %q11 : i8, i8
  }
}
