module attributes {pyc.frontend.contract = "pycircuit", pyc.top = @comb_dep_unknown_result} {
  func.func @comb_dep_unknown_result(%a: i8, %b: i8) -> i8 attributes {arg_names = ["a", "b"], pyc.base = "comb_dep_unknown_result", pyc.inline = "false", pyc.kind = "module", pyc.params = "{}", pyc.struct.collections = "[]", pyc.struct.metrics = "{\22ast_node_count\22:0,\22collection_count\22:0,\22collection_instance_count\22:0,\22estimated_inline_cost\22:0,\22hardware_call_count\22:0,\22instance_count\22:0,\22loop_count\22:0,\22module_call_count\22:0,\22module_family_collection_count\22:0,\22repeat_pressure\22:0,\22repeated_body_clusters\22:[],\22source_loc\22:0,\22state_alloc_count\22:0,\22state_call_count\22:0}", pyc.value_param_types = [], pyc.value_params = [], result_names = ["y"]} {
    // arith.addi is registered with MLIR, but is not part of pyCircuit's
    // canonical per-result dependency contract. It must fail closed instead
    // of silently acquiring an all-operands dependency approximation.
    %y = arith.addi %a, %b : i8
    return %y : i8
  }
}
