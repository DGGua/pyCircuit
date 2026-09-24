// pycc CLI fixture: the default policy must be structural.

module attributes {pyc.top = @state_delay_default_structural,
                   pyc.frontend.contract = "pycircuit"} {
  func.func @state_delay_default_structural(
      %clk: !pyc.clock, %rst: !pyc.reset, %in: i8) -> (i8, i8)
      attributes {arg_names = ["clk", "rst", "in"],
                  result_names = ["out0", "out1"],
                  pyc.kind = "module", pyc.inline = "false", pyc.params = "{}",
                  pyc.base = "state_delay_default_structural",
                  pyc.struct.metrics = "{\22ast_node_count\22:0,\22collection_count\22:0,\22collection_instance_count\22:0,\22estimated_inline_cost\22:0,\22hardware_call_count\22:0,\22instance_count\22:0,\22loop_count\22:0,\22module_call_count\22:0,\22module_family_collection_count\22:0,\22repeat_pressure\22:0,\22repeated_body_clusters\22:[],\22source_loc\22:0,\22state_alloc_count\22:0,\22state_call_count\22:0}",
                  pyc.struct.collections = "[]", pyc.value_params = [],
                  pyc.value_param_types = []} {
    %en = pyc.constant 1 : i1
    %init = pyc.constant 0 : i8
    %q0 = pyc.reg %clk, %rst, %en, %in, %init : i8
    %q1 = pyc.reg %clk, %rst, %en, %in, %init : i8
    func.return %q0, %q1 : i8, i8
  }
}
