module attributes {pyc.top = @top, pyc.frontend.contract = "pycircuit"} {
  func.func @leaf_small(%x: vector<2x3xi8>) -> vector<2x3xi8> attributes {arg_names = ["x"], result_names = ["y"], pyc.kind = "module", pyc.inline = "false", pyc.params = "{}", pyc.base = "leaf_small", pyc.struct.collections = "[]", pyc.struct.metrics = "{\"source_loc\":0,\"ast_node_count\":0,\"hardware_call_count\":0,\"loop_count\":0,\"module_call_count\":0,\"state_call_count\":0,\"estimated_inline_cost\":0,\"instance_count\":0,\"state_alloc_count\":0,\"collection_count\":0,\"collection_instance_count\":0,\"module_family_collection_count\":0,\"repeated_body_clusters\":[]}"} {
    %y = pyc.add %x, %x : vector<2x3xi8>, vector<2x3xi8> -> vector<2x3xi8>
    return %y : vector<2x3xi8>
  }
  func.func @leaf_wide(%x: vector<2x3xi1024>) -> vector<2x3xi1024> attributes {arg_names = ["x"], result_names = ["y"], pyc.kind = "module", pyc.inline = "false", pyc.params = "{}", pyc.base = "leaf_wide", pyc.struct.collections = "[]", pyc.struct.metrics = "{\"source_loc\":0,\"ast_node_count\":0,\"hardware_call_count\":0,\"loop_count\":0,\"module_call_count\":0,\"state_call_count\":0,\"estimated_inline_cost\":0,\"instance_count\":0,\"state_alloc_count\":0,\"collection_count\":0,\"collection_instance_count\":0,\"module_family_collection_count\":0,\"repeated_body_clusters\":[]}"} {
    %y = pyc.not %x : vector<2x3xi1024>
    return %y : vector<2x3xi1024>
  }
  func.func @top(%small: vector<2x3xi8>, %wide: vector<2x3xi1024>) -> (vector<2x3xi8>, vector<2x3xi1024>) attributes {arg_names = ["small","wide"], result_names = ["small_out","wide_out"], pyc.kind = "module", pyc.inline = "false", pyc.params = "{}", pyc.base = "top", pyc.struct.collections = "[]", pyc.struct.metrics = "{\"source_loc\":0,\"ast_node_count\":0,\"hardware_call_count\":0,\"loop_count\":0,\"module_call_count\":2,\"state_call_count\":0,\"estimated_inline_cost\":0,\"instance_count\":2,\"state_alloc_count\":0,\"collection_count\":0,\"collection_instance_count\":0,\"module_family_collection_count\":0,\"repeated_body_clusters\":[]}"} {
    %small_out = pyc.instance %small {callee = @leaf_small, name = "u_small"} : (vector<2x3xi8>) -> vector<2x3xi8>
    %wide_out = pyc.instance %wide {callee = @leaf_wide, name = "u_wide"} : (vector<2x3xi1024>) -> vector<2x3xi1024>
    return %small_out, %wide_out : vector<2x3xi8>, vector<2x3xi1024>
  }
}
