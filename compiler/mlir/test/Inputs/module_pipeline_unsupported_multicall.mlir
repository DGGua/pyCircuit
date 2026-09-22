module attributes {pyc.top = @Top, pyc.frontend.contract = "pycircuit"} {
  func.func @A(%forward : i1, %feedback : i1) -> i1
      attributes {arg_names = ["forward", "feedback"], result_names = ["out"], pyc.kind = "module", pyc.inline = "false", pyc.params = "{}", pyc.base = "A", pyc.struct.metrics = "{\"ast_node_count\":0,\"collection_count\":0,\"collection_instance_count\":0,\"estimated_inline_cost\":0,\"hardware_call_count\":0,\"instance_count\":0,\"loop_count\":0,\"module_call_count\":0,\"module_family_collection_count\":0,\"repeat_pressure\":0,\"repeated_body_clusters\":[],\"source_loc\":0,\"state_alloc_count\":0,\"state_call_count\":0}", pyc.struct.collections = "[]", pyc.value_params = [], pyc.value_param_types = []} {
    return %forward : i1
  }

  func.func @B(%forward : i1, %feedback : i1) -> i1
      attributes {arg_names = ["forward", "feedback"], result_names = ["out"], pyc.kind = "module", pyc.inline = "false", pyc.params = "{}", pyc.base = "B", pyc.struct.metrics = "{\"ast_node_count\":0,\"collection_count\":0,\"collection_instance_count\":0,\"estimated_inline_cost\":0,\"hardware_call_count\":0,\"instance_count\":0,\"loop_count\":0,\"module_call_count\":0,\"module_family_collection_count\":0,\"repeat_pressure\":0,\"repeated_body_clusters\":[],\"source_loc\":0,\"state_alloc_count\":0,\"state_call_count\":0}", pyc.struct.collections = "[]", pyc.value_params = [], pyc.value_param_types = []} {
    return %forward : i1
  }

  func.func @Top() -> i1
      attributes {arg_names = [], result_names = ["extra"], pyc.kind = "module", pyc.inline = "false", pyc.params = "{}", pyc.base = "Top", pyc.struct.metrics = "{\"ast_node_count\":0,\"collection_count\":0,\"collection_instance_count\":0,\"estimated_inline_cost\":0,\"hardware_call_count\":0,\"instance_count\":3,\"loop_count\":0,\"module_call_count\":3,\"module_family_collection_count\":0,\"repeat_pressure\":0,\"repeated_body_clusters\":[],\"source_loc\":0,\"state_alloc_count\":0,\"state_call_count\":0}", pyc.struct.collections = "[]", pyc.value_params = [], pyc.value_param_types = []} {
    %zero = arith.constant 0 : i1
    %a_feedback = pyc.wire {pyc.name = "a_feedback"} : i1
    %b_feedback = pyc.wire {pyc.name = "b_feedback"} : i1
    %a = pyc.instance %zero, %b_feedback
        {callee = @A, name = "a"} : (i1, i1) -> i1
    %b = pyc.instance %zero, %a_feedback
        {callee = @B, name = "b"} : (i1, i1) -> i1
    %extra = pyc.instance %zero, %zero
        {callee = @A, name = "extra"} : (i1, i1) -> i1
    pyc.assign %a_feedback, %a : i1
    pyc.assign %b_feedback, %b : i1
    return %extra : i1
  }
}
