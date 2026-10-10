// Probe: fuse-comb graph mode scratch-state residue across BLOCKS of one
// function (the pass instance is per-function, so block 2 sees block 1's
// erased ops in the member `ops` vector).
module attributes {pyc.top = @Top, pyc.frontend.contract = "pycircuit"} {
  func.func @Top(%a : i8, %b : i8) -> (i8, i8) attributes {arg_names = ["a", "b"], result_names = ["o1", "o2"], pyc.kind = "module", pyc.inline = "false", pyc.params = "{}", pyc.base = "Top", pyc.struct.metrics = "{\"ast_node_count\":0,\"collection_count\":0,\"collection_instance_count\":0,\"estimated_inline_cost\":0,\"hardware_call_count\":0,\"instance_count\":0,\"loop_count\":0,\"module_call_count\":0,\"module_family_collection_count\":0,\"repeat_pressure\":0,\"repeated_body_clusters\":[],\"source_loc\":0,\"state_alloc_count\":0,\"state_call_count\":0}", pyc.struct.collections = "[]", pyc.value_params = [], pyc.value_param_types = []} {
    %c = pyc.and %a, %b : i8, i8 -> i8
    %d = pyc.xor %c, %a : i8, i8 -> i8
    cf.br ^bb1(%c, %d : i8, i8)
  ^bb1(%x : i8, %y : i8):
    %e = pyc.or %x, %y : i8, i8 -> i8
    return %e, %d : i8, i8
  }
}
