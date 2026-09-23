module attributes {pyc.frontend.contract = "pycircuit", pyc.top = @comb_dep_structural_top} {
  // Structural @function helpers are inlined before graph construction. The
  // supported arith.select they contain remains an explicit graph transfer.
  func.func @select_helper(%sel: i1, %a: i8, %b: i8) -> i8 attributes {arg_names = ["sel", "a", "b"], pyc.base = "select_helper", pyc.inline = "true", pyc.kind = "function", pyc.params = "{}", pyc.struct.collections = "[]", pyc.struct.metrics = "{\22ast_node_count\22:0,\22collection_count\22:0,\22collection_instance_count\22:0,\22estimated_inline_cost\22:1,\22hardware_call_count\22:1,\22instance_count\22:0,\22loop_count\22:0,\22module_call_count\22:0,\22module_family_collection_count\22:0,\22repeat_pressure\22:0,\22repeated_body_clusters\22:[],\22source_loc\22:0,\22state_alloc_count\22:0,\22state_call_count\22:0}", pyc.value_param_types = [], pyc.value_params = [], result_names = ["y"]} {
    %y = arith.select %sel, %a, %b : i8
    return %y : i8
  }

  func.func @comb_dep_structural_top(%sel: i1, %a: i8, %b: i8) -> i8 attributes {arg_names = ["sel", "a", "b"], pyc.base = "comb_dep_structural_top", pyc.inline = "false", pyc.kind = "module", pyc.params = "{}", pyc.struct.collections = "[]", pyc.struct.metrics = "{\22ast_node_count\22:0,\22collection_count\22:0,\22collection_instance_count\22:0,\22estimated_inline_cost\22:1,\22hardware_call_count\22:1,\22instance_count\22:1,\22loop_count\22:0,\22module_call_count\22:1,\22module_family_collection_count\22:0,\22repeat_pressure\22:0,\22repeated_body_clusters\22:[],\22source_loc\22:0,\22state_alloc_count\22:0,\22state_call_count\22:0}", pyc.value_param_types = [], pyc.value_params = [], result_names = ["y"]} {
    %y = pyc.instance %sel, %a, %b {callee = @select_helper, name = "u_select"} : (i1, i8, i8) -> i8
    return %y : i8
  }
}
