module attributes {pyc.top = @comb_host_boundary, pyc.frontend.contract = "pycircuit"} {
  func.func @comb_host_boundary(%a: i8, %unused: i8) -> (i8, i8) attributes {
    arg_names = ["a", "unused"],
    result_names = ["lhs", "rhs"],
    pyc.kind = "module",
    pyc.inline = "false",
    pyc.params = "{}",
    pyc.base = "build",
    pyc.struct.metrics = "{\"ast_node_count\":0,\"collection_count\":0,\"collection_instance_count\":0,\"estimated_inline_cost\":0,\"hardware_call_count\":0,\"instance_count\":0,\"loop_count\":0,\"module_call_count\":0,\"module_family_collection_count\":0,\"repeat_pressure\":0,\"repeated_body_clusters\":[],\"source_loc\":0,\"state_alloc_count\":0,\"state_call_count\":0}",
    pyc.struct.collections = "[]",
    pyc.value_params = [],
    pyc.value_param_types = []
  } {
    %0:2 = pyc.comb(%a, %a, %unused) : (i8, i8, i8) -> (i8, i8) {
    ^bb0(%first: i8, %duplicate: i8, %dead: i8):
      pyc.yield %duplicate, %duplicate : i8, i8
    }
    %1 = pyc.comb(%0#0) : (i8) -> i8 {
    ^bb0(%value: i8):
      %named = pyc.alias %value {pyc.name = "observed"} : i8
      pyc.yield %named : i8
    }
    func.return %1, %0#1 : i8, i8
  }
}
