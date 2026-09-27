module attributes {pyc.top = @top, pyc.frontend.contract = "pycircuit"} {
  func.func @top(%sel: i1, %a: vector<3xi8>, %b: vector<3xi8>, %x: i8, %z: i8) -> (i8, i8, i8, i8) attributes {arg_names = ["sel", "a", "b", "x", "z"], result_names = ["added", "selected", "created", "broadcasted"], pyc.kind = "module", pyc.inline = "false", pyc.params = "{}", pyc.base = "top", pyc.struct.metrics = "{\"source_loc\":0,\"ast_node_count\":0,\"hardware_call_count\":0,\"loop_count\":0,\"module_call_count\":0,\"state_call_count\":1,\"estimated_inline_cost\":0,\"instance_count\":0,\"state_alloc_count\":1,\"collection_count\":0,\"collection_instance_count\":0,\"module_family_collection_count\":0,\"repeated_body_clusters\":[]}", pyc.struct.collections = "[]"} {
    %sum = pyc.add %a, %b {pyc.name = "aggregate_add"} : vector<3xi8>, vector<3xi8> -> vector<3xi8>
    %added = pyc.v_get %sum[1] : vector<3xi8> -> i8
    %mux = pyc.mux %sel, %a, %b {pyc.name = "aggregate_mux"} : i1, vector<3xi8>, vector<3xi8> -> vector<3xi8>
    %selected = pyc.v_get %mux[1] : vector<3xi8> -> i8
    %vec = pyc.v_create (%x, %z) {pyc.name = "aggregate_create"} : (i8, i8) -> vector<2xi8>
    %created = pyc.v_get %vec[1] : vector<2xi8> -> i8
    %broadcast = pyc.v_broadcast %x to 3 {pyc.debug_keep} : i8 -> vector<3xi8>
    %broadcasted = pyc.v_get %broadcast[2] : vector<3xi8> -> i8
    return %added, %selected, %created, %broadcasted : i8, i8, i8, i8
  }
}
