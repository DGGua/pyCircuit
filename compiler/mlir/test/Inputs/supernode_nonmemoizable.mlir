// A generic pure op must not become memoizable merely because it has no
// memory effects. The explicit comb whitelist is the runtime equality and
// determinism contract shared by fusion and optional update.
module attributes {pyc.frontend.contract = "pycircuit", pyc.top = @nonmemoizable} {
  func.func @nonmemoizable(%arg0: i8, %arg1: i8) -> i8 attributes {arg_names = ["a", "b"], pyc.base = "nonmemoizable", pyc.inline = "false", pyc.kind = "module", pyc.params = "{}", pyc.struct.collections = "[]", pyc.struct.metrics = "{\22ast_node_count\22:0,\22collection_count\22:0,\22collection_instance_count\22:0,\22estimated_inline_cost\22:0,\22hardware_call_count\22:0,\22instance_count\22:0,\22loop_count\22:0,\22module_call_count\22:0,\22module_family_collection_count\22:0,\22repeat_pressure\22:0,\22repeated_body_clusters\22:[],\22source_loc\22:0,\22state_alloc_count\22:0,\22state_call_count\22:0}", pyc.value_param_types = [], pyc.value_params = [], result_names = ["y"]} {
    %0 = pyc.comb(%arg0, %arg1) : (i8, i8) -> i8 {
    ^bb0(%arg2: i8, %arg3: i8):
      %1 = arith.addi %arg2, %arg3 : i8
      pyc.yield %1 : i8
    }
    return %0 : i8
  }
}
