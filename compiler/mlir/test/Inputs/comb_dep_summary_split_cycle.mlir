module attributes {pyc.top = @top, pyc.frontend.contract = "pycircuit"} {
  func.func private @id(i1) -> i1 attributes {arg_names = ["x"], result_names = ["y"], pyc.kind = "module", pyc.inline = "false", pyc.params = "{}", pyc.base = "id", pyc.struct.metrics = "{\"source_loc\":0,\"ast_node_count\":0,\"hardware_call_count\":0,\"loop_count\":0,\"module_call_count\":0,\"state_call_count\":0,\"estimated_inline_cost\":0,\"instance_count\":0,\"state_alloc_count\":0,\"collection_count\":0,\"collection_instance_count\":0,\"module_family_collection_count\":0,\"repeated_body_clusters\":[]}", pyc.struct.collections = "[]", pyc.value_params = [], pyc.value_param_types = []}

  func.func @top(%x: i1) -> i1 attributes {arg_names = ["x"], result_names = ["y"], pyc.kind = "module", pyc.inline = "false", pyc.params = "{}", pyc.base = "top", pyc.struct.metrics = "{\"source_loc\":0,\"ast_node_count\":0,\"hardware_call_count\":0,\"loop_count\":0,\"module_call_count\":1,\"state_call_count\":0,\"estimated_inline_cost\":0,\"instance_count\":1,\"state_alloc_count\":0,\"collection_count\":0,\"collection_instance_count\":0,\"module_family_collection_count\":0,\"repeated_body_clusters\":[]}", pyc.struct.collections = "[]", pyc.value_params = [], pyc.value_param_types = []} {
    %w = pyc.wire {pyc.name = "w"} : i1
    %y = pyc.instance %w {callee = @id, name = "u_id"} : (i1) -> i1
    pyc.assign %w, %y : i1
    return %w : i1
  }
}
