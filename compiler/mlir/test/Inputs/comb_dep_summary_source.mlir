module attributes {pyc.top = @id, pyc.frontend.contract = "pycircuit"} {
  func.func @id(%x: i1) -> i1 attributes {arg_names = ["x"], result_names = ["y"], pyc.kind = "module", pyc.inline = "false", pyc.params = "{}", pyc.base = "id", pyc.struct.metrics = "{\"source_loc\":0,\"ast_node_count\":0,\"hardware_call_count\":0,\"loop_count\":0,\"module_call_count\":0,\"state_call_count\":0,\"estimated_inline_cost\":0,\"instance_count\":0,\"state_alloc_count\":0,\"collection_count\":0,\"collection_instance_count\":0,\"module_family_collection_count\":0,\"repeated_body_clusters\":[]}", pyc.struct.collections = "[]", pyc.value_params = [], pyc.value_param_types = []} {
    return %x : i1
  }

  func.func @const_out(%x: i1) -> i1 attributes {arg_names = ["x"], result_names = ["y"], pyc.kind = "module", pyc.inline = "false", pyc.params = "{}", pyc.base = "const_out", pyc.struct.metrics = "{\"source_loc\":0,\"ast_node_count\":0,\"hardware_call_count\":1,\"loop_count\":0,\"module_call_count\":0,\"state_call_count\":0,\"estimated_inline_cost\":0,\"instance_count\":0,\"state_alloc_count\":0,\"collection_count\":0,\"collection_instance_count\":0,\"module_family_collection_count\":0,\"repeated_body_clusters\":[]}", pyc.struct.collections = "[]", pyc.value_params = [], pyc.value_param_types = []} {
    %one = pyc.constant 1 : i1
    return %one : i1
  }

  func.func @chain4(%x: i8) -> i8 attributes {arg_names = ["x"], result_names = ["y"], pyc.kind = "module", pyc.inline = "false", pyc.params = "{}", pyc.base = "chain4", pyc.struct.metrics = "{\"source_loc\":0,\"ast_node_count\":0,\"hardware_call_count\":8,\"loop_count\":0,\"module_call_count\":0,\"state_call_count\":0,\"estimated_inline_cost\":0,\"instance_count\":0,\"state_alloc_count\":0,\"collection_count\":0,\"collection_instance_count\":0,\"module_family_collection_count\":0,\"repeated_body_clusters\":[]}", pyc.struct.collections = "[]", pyc.value_params = [], pyc.value_param_types = []} {
    %c0 = pyc.constant 13 : i8
    %c1 = pyc.constant 37 : i8
    %c2 = pyc.constant 240 : i8
    %c3 = pyc.constant 85 : i8
    %t0 = pyc.add %x, %c0 : i8, i8 -> i8
    %t1 = pyc.xor %t0, %c1 : i8, i8 -> i8
    %t2 = pyc.and %t1, %c2 : i8, i8 -> i8
    %t3 = pyc.or %t2, %c3 : i8, i8 -> i8
    return %t3 : i8
  }
}
