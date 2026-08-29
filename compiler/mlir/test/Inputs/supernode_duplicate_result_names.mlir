module attributes {pyc.frontend.contract = "pycircuit", pyc.top = @supernode_duplicate_result_names} {
  func.func @supernode_duplicate_result_names(%a: i8) -> (i8, i8) attributes {arg_names = ["a"], pyc.base = "supernode_duplicate_result_names", pyc.inline = "false", pyc.kind = "module", pyc.params = "{}", pyc.struct.collections = "[]", pyc.struct.metrics = "{\22ast_node_count\22:0,\22collection_count\22:0,\22collection_instance_count\22:0,\22estimated_inline_cost\22:0,\22hardware_call_count\22:0,\22instance_count\22:0,\22loop_count\22:0,\22module_call_count\22:0,\22module_family_collection_count\22:0,\22repeat_pressure\22:0,\22repeated_body_clusters\22:[],\22source_loc\22:0,\22state_alloc_count\22:0,\22state_call_count\22:0}", pyc.value_param_types = [], pyc.value_params = [], result_names = ["y0", "y1"]} {
    %0:2 = pyc.comb(%a) {pyc.comb.result_names = ["tap_a", "tap_b"]} : (i8) -> (i8, i8) {
    ^bb0(%in: i8):
      %v = pyc.not %in : i8
      pyc.yield %v, %v : i8, i8
    }
    return %0#0, %0#1 : i8, i8
  }
}
