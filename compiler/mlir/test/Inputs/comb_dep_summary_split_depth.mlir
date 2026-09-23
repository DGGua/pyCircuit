module attributes {pyc.top = @top, pyc.frontend.contract = "pycircuit"} {
  func.func private @chain4(i8) -> i8 attributes {arg_names = ["x"], result_names = ["y"], pyc.kind = "module", pyc.inline = "false", pyc.params = "{}", pyc.base = "chain4", pyc.struct.metrics = "{\"source_loc\":0,\"ast_node_count\":0,\"hardware_call_count\":8,\"loop_count\":0,\"module_call_count\":0,\"state_call_count\":0,\"estimated_inline_cost\":0,\"instance_count\":0,\"state_alloc_count\":0,\"collection_count\":0,\"collection_instance_count\":0,\"module_family_collection_count\":0,\"repeated_body_clusters\":[]}", pyc.struct.collections = "[]", pyc.value_params = [], pyc.value_param_types = []}

  func.func @top(%x: i8) -> i8 attributes {arg_names = ["x"], result_names = ["y"], pyc.kind = "module", pyc.inline = "false", pyc.params = "{}", pyc.base = "top", pyc.struct.metrics = "{\"source_loc\":0,\"ast_node_count\":0,\"hardware_call_count\":0,\"loop_count\":0,\"module_call_count\":2,\"state_call_count\":0,\"estimated_inline_cost\":0,\"instance_count\":2,\"state_alloc_count\":0,\"collection_count\":0,\"collection_instance_count\":0,\"module_family_collection_count\":0,\"repeated_body_clusters\":[]}", pyc.struct.collections = "[]", pyc.value_params = [], pyc.value_param_types = []} {
    %y0 = pyc.instance %x {callee = @chain4, name = "u0"} : (i8) -> i8
    %y1 = pyc.instance %y0 {callee = @chain4, name = "u1"} : (i8) -> i8
    return %y1 : i8
  }
}
