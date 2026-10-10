// Probe: fuse-comb graph mode scratch-state residue across functions.
// @Sub fuses its two ops into a region and erases them. The pass keeps
// `ops` as a member and never clears it, so processing @Top afterwards
// walks dangling Operation* pointers from @Sub.
module attributes {pyc.top = @Top, pyc.frontend.contract = "pycircuit"} {
  func.func @Sub(%x : i8) -> (i8) attributes {arg_names = ["x"], result_names = ["r"], pyc.kind = "module", pyc.inline = "false", pyc.params = "{}", pyc.base = "Sub", pyc.struct.metrics = "{\"ast_node_count\":0,\"collection_count\":0,\"collection_instance_count\":0,\"estimated_inline_cost\":0,\"hardware_call_count\":0,\"instance_count\":0,\"loop_count\":0,\"module_call_count\":0,\"module_family_collection_count\":0,\"repeat_pressure\":0,\"repeated_body_clusters\":[],\"source_loc\":0,\"state_alloc_count\":0,\"state_call_count\":0}", pyc.struct.collections = "[]", pyc.value_params = [], pyc.value_param_types = []} {
    %one = pyc.constant 1 : i8
    %r = pyc.add %x, %one : i8, i8 -> i8
    return %r : i8
  }

  func.func @Top(%a : i8, %b : i8) -> (i8) attributes {arg_names = ["a", "b"], result_names = ["y"], pyc.kind = "module", pyc.inline = "false", pyc.params = "{}", pyc.base = "Top", pyc.struct.metrics = "{\"ast_node_count\":0,\"collection_count\":0,\"collection_instance_count\":0,\"estimated_inline_cost\":0,\"hardware_call_count\":0,\"instance_count\":0,\"loop_count\":0,\"module_call_count\":0,\"module_family_collection_count\":0,\"repeat_pressure\":0,\"repeated_body_clusters\":[],\"source_loc\":0,\"state_alloc_count\":0,\"state_call_count\":0}", pyc.struct.collections = "[]", pyc.value_params = [], pyc.value_param_types = []} {
    %c = pyc.and %a, %b : i8, i8 -> i8
    %d = pyc.xor %c, %a : i8, i8 -> i8
    %y = pyc.or %c, %d : i8, i8 -> i8
    return %y : i8
  }
}
