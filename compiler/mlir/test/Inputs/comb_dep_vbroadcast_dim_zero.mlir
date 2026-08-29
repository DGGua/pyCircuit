module attributes {pyc.frontend.contract = "pycircuit", pyc.top = @comb_dep_vbroadcast_dim_zero} {
  func.func @comb_dep_vbroadcast_dim_zero(%x: vector<2xi8>) -> vector<3x2xi8> attributes {arg_names = ["x"], pyc.base = "comb_dep_vbroadcast_dim_zero", pyc.inline = "false", pyc.kind = "module", pyc.params = "{}", pyc.struct.collections = "[]", pyc.struct.metrics = "{\22ast_node_count\22:0,\22collection_count\22:0,\22collection_instance_count\22:0,\22estimated_inline_cost\22:0,\22hardware_call_count\22:1,\22instance_count\22:0,\22loop_count\22:0,\22module_call_count\22:0,\22module_family_collection_count\22:0,\22repeat_pressure\22:0,\22repeated_body_clusters\22:[],\22source_loc\22:0,\22state_alloc_count\22:0,\22state_call_count\22:0}", pyc.value_param_types = [], pyc.value_params = [], result_names = ["y"]} {
    %y = pyc.v_broadcast_dim %x to 3, 0 : vector<2xi8> -> vector<3x2xi8>
    return %y : vector<3x2xi8>
  }
}
