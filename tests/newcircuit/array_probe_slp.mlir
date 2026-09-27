module attributes {pyc.top = @top, pyc.frontend.contract = "pycircuit"} {
  func.func @top(%a: i8, %b: i8, %c: i8, %d: i8, %s: i1, %t: i1) -> (vector<2xi8>, vector<2xi8>, vector<2xi8>, vector<2xi1>) attributes {arg_names = ["a", "b", "c", "d", "s", "t"], result_names = ["xors", "nots", "muxes", "equals"], pyc.kind = "module", pyc.inline = "false", pyc.params = "{}", pyc.base = "top", pyc.struct.metrics = "{\"source_loc\":0,\"ast_node_count\":0,\"hardware_call_count\":0,\"loop_count\":0,\"module_call_count\":0,\"state_call_count\":1,\"estimated_inline_cost\":0,\"instance_count\":0,\"state_alloc_count\":1,\"collection_count\":0,\"collection_instance_count\":0,\"module_family_collection_count\":0,\"repeated_body_clusters\":[]}", pyc.struct.collections = "[]"} {
    %x0 = pyc.xor %a, %b {pyc.name = "xor0"} : i8, i8 -> i8
    %x1 = pyc.xor %c, %d {pyc.name = "xor1"} : i8, i8 -> i8
    %xors = pyc.v_create (%x0, %x1) : (i8, i8) -> vector<2xi8>
    %n0 = pyc.not %a {pyc.name = "not0"} : i8
    %n1 = pyc.not %c {pyc.name = "not1"} : i8
    %nots = pyc.v_create (%n0, %n1) : (i8, i8) -> vector<2xi8>
    %m0 = pyc.mux %s, %a, %b {pyc.name = "mux0"} : i1, i8, i8 -> i8
    %m1 = pyc.mux %t, %c, %d {pyc.name = "mux1"} : i1, i8, i8 -> i8
    %muxes = pyc.v_create (%m0, %m1) : (i8, i8) -> vector<2xi8>
    %e0 = pyc.eq %a, %b : i8, i8 -> i1
    %e1 = pyc.eq %c, %d : i8, i8 -> i1
    %equals = pyc.v_create (%e0, %e1) {pyc.name = "equal_pair"} : (i1, i1) -> vector<2xi1>
    return %xors, %nots, %muxes, %equals : vector<2xi8>, vector<2xi8>, vector<2xi8>, vector<2xi1>
  }
}
