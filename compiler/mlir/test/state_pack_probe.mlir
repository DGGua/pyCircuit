// pycc fixture for packed-state output probe classification and slice metadata.

module attributes {pyc.top = @state_pack_probe,
                   pyc.frontend.contract = "pycircuit"} {
  func.func @state_pack_probe(
      %clk: !pyc.clock, %rst: !pyc.reset, %en: i1, %a: i8, %b: i8)
      -> (i8, i8)
      attributes {arg_names = ["clk", "rst", "en", "a", "b"],
                  result_names = ["out0", "out1"],
                  pyc.kind = "module", pyc.inline = "false", pyc.params = "{}",
                  pyc.base = "state_pack_probe",
                  pyc.struct.metrics = "{\22ast_node_count\22:0,\22collection_count\22:0,\22collection_instance_count\22:0,\22estimated_inline_cost\22:0,\22hardware_call_count\22:0,\22instance_count\22:0,\22loop_count\22:0,\22module_call_count\22:0,\22module_family_collection_count\22:0,\22repeat_pressure\22:0,\22repeated_body_clusters\22:[],\22source_loc\22:0,\22state_alloc_count\22:0,\22state_call_count\22:0}",
                  pyc.struct.collections = "[]", pyc.value_params = [],
                  pyc.value_param_types = []} {
    %init0 = pyc.constant 17 : i8
    %init1 = pyc.constant 34 : i8
    %q0 = pyc.reg %clk, %rst, %en, %a, %init0 {pyc.name = "lane0_state"} : i8
    %q1 = pyc.reg %clk, %rst, %en, %b, %init1 {pyc.name = "lane1_state"} : i8
    func.return %q0, %q1 : i8, i8
  }
}
