// The partition verifier must reject part_id == part_count.  Keeping this as a
// direct MLIR fixture makes the negative gate independent of the partition
// rewrite, which would otherwise replace malformed user metadata.
module attributes {pyc.frontend.contract = "pycircuit", pyc.top = @invalid_partition} {
  func.func @invalid_partition(%arg0: i8) -> i8 attributes {arg_names = ["a"], pyc.base = "invalid_partition", pyc.inline = "false", pyc.kind = "module", pyc.params = "{}", pyc.struct.collections = "[]", pyc.struct.metrics = "{\22ast_node_count\22:0,\22collection_count\22:0,\22collection_instance_count\22:0,\22estimated_inline_cost\22:0,\22hardware_call_count\22:0,\22instance_count\22:0,\22loop_count\22:0,\22module_call_count\22:0,\22module_family_collection_count\22:0,\22repeat_pressure\22:0,\22repeated_body_clusters\22:[],\22source_loc\22:0,\22state_alloc_count\22:0,\22state_call_count\22:0}", pyc.value_param_types = [], pyc.value_params = [], result_names = ["y"]} {
    %0 = pyc.comb(%arg0) {pyc.partition.max_nodes = 1 : i64, pyc.partition.parent_id = 0 : i64, pyc.partition.part_count = 1 : i64, pyc.partition.part_id = 1 : i64, pyc.partition.plan_version = "gsim-contiguous-dp-v1", pyc.partition.work = 1 : i64} : (i8) -> i8 {
    ^bb0(%arg1: i8):
      %1 = pyc.not %arg1 : i8
      pyc.yield %1 : i8
    }
    return %0 : i8
  }
}
