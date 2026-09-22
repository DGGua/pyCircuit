module attributes {pyc.top = @StateCut, pyc.frontend.contract = "pycircuit"} {
  func.func @StateCut(%clk : !pyc.clock, %rst : !pyc.reset,
                      %enable : i1, %next : i8, %init : i8) -> i8
      attributes {arg_names = ["clk", "rst", "enable", "next", "init"], result_names = ["q"], pyc.kind = "module", pyc.inline = "false", pyc.params = "{}", pyc.base = "StateCut", pyc.struct.metrics = "{\"ast_node_count\":0,\"collection_count\":0,\"collection_instance_count\":0,\"estimated_inline_cost\":0,\"hardware_call_count\":0,\"instance_count\":0,\"loop_count\":0,\"module_call_count\":0,\"module_family_collection_count\":0,\"repeat_pressure\":0,\"repeated_body_clusters\":[],\"source_loc\":0,\"state_alloc_count\":1,\"state_call_count\":1}", pyc.struct.collections = "[]", pyc.value_params = [], pyc.value_param_types = []} {
    %q = pyc.reg %clk, %rst, %enable, %next, %init : i8
    return %q : i8
  }
}
