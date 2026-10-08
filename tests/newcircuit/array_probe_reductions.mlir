module attributes {pyc.top = @top, pyc.frontend.contract = "pycircuit"} {
  func.func @top(%v: vector<3x4xi8>, %r: vector<4xi8>) -> (i8, i8, i8, i8, i8) attributes {arg_names = ["v", "r"], result_names = ["or0", "and1", "sum2", "row2", "col1"], pyc.kind = "module", pyc.inline = "false", pyc.params = "{}", pyc.base = "top", pyc.struct.metrics = "{\"source_loc\":0,\"ast_node_count\":0,\"hardware_call_count\":0,\"loop_count\":0,\"module_call_count\":0,\"state_call_count\":1,\"estimated_inline_cost\":0,\"instance_count\":0,\"state_alloc_count\":1,\"collection_count\":0,\"collection_instance_count\":0,\"module_family_collection_count\":0,\"repeated_body_clusters\":[]}", pyc.struct.collections = "[]"} {
    %ors = pyc.v_or_reduce %v {dim = 1 : i64, mode = "chain"} : vector<3x4xi8> -> vector<3xi8>
    %ands = pyc.v_and_reduce %v {dim = 1 : i64, mode = "tree"} : vector<3x4xi8> -> vector<3xi8>
    %sums = pyc.v_add_reduce %v {dim = 0 : i64, mode = "chain"} : vector<3x4xi8> -> vector<4xi8>
    %or0 = pyc.v_get %ors[0] {pyc.name = "or_probe"} : vector<3xi8> -> i8
    %and1 = pyc.v_get %ands[1] {pyc.name = "and_probe"} : vector<3xi8> -> i8
    %sum2 = pyc.v_get %sums[2] {pyc.name = "sum_probe"} : vector<4xi8> -> i8
    %rows = pyc.v_broadcast_dim %r to 3, 0 : vector<4xi8> -> vector<3x4xi8>
    %row = pyc.v_get %rows[1] {pyc.name = "row_probe"} : vector<3x4xi8> -> vector<4xi8>
    %row2 = pyc.v_get %row[2] : vector<4xi8> -> i8
    %columns = pyc.v_broadcast_dim %r to 2, 1 : vector<4xi8> -> vector<4x2xi8>
    %column = pyc.v_get %columns[2] {pyc.name = "column_probe"} : vector<4x2xi8> -> vector<2xi8>
    %col1 = pyc.v_get %column[1] : vector<2xi8> -> i8
    return %or0, %and1, %sum2, %row2, %col1 : i8, i8, i8, i8, i8
  }
}
