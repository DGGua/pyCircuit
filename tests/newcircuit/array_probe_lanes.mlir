module attributes {pyc.top = @top, pyc.frontend.contract = "pycircuit"} {
  func.func @top(%clk: !pyc.clock, %rst: !pyc.reset, %en: i1, %d: vector<2x3xi8>, %init: vector<2x3xi8>, %a: vector<2x3xi8>, %x: i8, %z: i8) -> (i8, i8, i8, i8, i8, i8) attributes {arg_names = ["clk", "rst", "en", "d", "init", "a", "x", "z"], result_names = ["lane01", "duplicate01", "lane12", "created", "broadcasted", "state01"], pyc.kind = "module", pyc.inline = "false", pyc.params = "{}", pyc.base = "top", pyc.struct.metrics = "{\"source_loc\":0,\"ast_node_count\":0,\"hardware_call_count\":0,\"loop_count\":0,\"module_call_count\":0,\"state_call_count\":1,\"estimated_inline_cost\":0,\"instance_count\":0,\"state_alloc_count\":1,\"collection_count\":0,\"collection_instance_count\":0,\"module_family_collection_count\":0,\"repeated_body_clusters\":[]}", pyc.struct.collections = "[]"} {
    %q = pyc.reg %clk, %rst, %en, %d, %init : vector<2x3xi8>
    %state_row = pyc.v_get %q[0] : vector<2x3xi8> -> vector<3xi8>
    %state01 = pyc.v_get %state_row[1] {pyc.name = "state_probe"} : vector<3xi8> -> i8
    %sum = pyc.add %q, %a : vector<2x3xi8>, vector<2x3xi8> -> vector<2x3xi8>
    %row0 = pyc.v_get %sum[0] : vector<2x3xi8> -> vector<3xi8>
    %row1 = pyc.v_get %sum[1] : vector<2x3xi8> -> vector<3xi8>
    %lane01 = pyc.v_get %row0[1] {pyc.name = "sum_probe", pyc.debug_keep} : vector<3xi8> -> i8
    %duplicate01 = pyc.v_get %row0[1] {pyc.name = "duplicate_probe"} : vector<3xi8> -> i8
    %lane12 = pyc.v_get %row1[2] {pyc.debug_keep} : vector<3xi8> -> i8
    %vec = pyc.v_create (%x, %z) : (i8, i8) -> vector<2xi8>
    %created = pyc.v_get %vec[1] {pyc.name = "created_probe"} : vector<2xi8> -> i8
    %broadcast = pyc.v_broadcast %x to 3 : i8 -> vector<3xi8>
    %broadcasted = pyc.v_get %broadcast[2] {pyc.name = "broadcast_probe"} : vector<3xi8> -> i8
    return %lane01, %duplicate01, %lane12, %created, %broadcasted, %state01 : i8, i8, i8, i8, i8, i8
  }
}
