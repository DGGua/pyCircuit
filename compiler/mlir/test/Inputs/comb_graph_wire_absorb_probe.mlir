// Probe: single-reader wire transparency in fuse-comb graph mode.
// %w is driven by one assign and read only by %z (after the assign); the
// partition graph must route %z's dependency through the wire so the mux
// cone and %z merge into one comb, while the wire/assign stay in the block.
module attributes {pyc.top = @Top, pyc.frontend.contract = "pycircuit"} {
  func.func @Top(%a : i8, %b : i8, %en : i1) -> (i8, i8) attributes {arg_names = ["a", "b", "en"], result_names = ["z", "y"], pyc.kind = "module", pyc.inline = "false", pyc.params = "{}", pyc.base = "Top", pyc.struct.metrics = "{\"ast_node_count\":0,\"collection_count\":0,\"collection_instance_count\":0,\"estimated_inline_cost\":0,\"hardware_call_count\":0,\"instance_count\":0,\"loop_count\":0,\"module_call_count\":0,\"module_family_collection_count\":0,\"repeat_pressure\":0,\"repeated_body_clusters\":[],\"source_loc\":0,\"state_alloc_count\":0,\"state_call_count\":0}", pyc.struct.collections = "[]", pyc.value_params = [], pyc.value_param_types = []} {
    %c = pyc.and %a, %b : i8, i8 -> i8
    %one = pyc.constant 1 : i8
    %inc = pyc.add %c, %one : i8, i8 -> i8
    %y = pyc.mux %en, %inc, %c : i1, i8, i8 -> i8
    %w = pyc.wire {pyc.name = "y_wire"} : i8
    pyc.assign %w, %y : i8
    %z = pyc.xor %w, %a : i8, i8 -> i8
    return %z, %w : i8, i8
  }
}
