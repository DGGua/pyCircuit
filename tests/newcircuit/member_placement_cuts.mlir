module attributes {pyc.top = @top, pyc.frontend.contract = "pycircuit"} {
  func.func @top(%a: i8, %b: i16, %c: i64, %d: i128) -> (i8, i16, i64, i128) attributes {arg_names = ["a", "b", "c", "d"], result_names = ["oa", "ob", "oc", "od"], pyc.kind = "module", pyc.inline = "false", pyc.params = "{}", pyc.base = "top", pyc.struct.metrics = "{\"source_loc\":0,\"ast_node_count\":0,\"hardware_call_count\":0,\"loop_count\":0,\"module_call_count\":0,\"state_call_count\":0,\"estimated_inline_cost\":0,\"instance_count\":0,\"state_alloc_count\":0,\"collection_count\":0,\"collection_instance_count\":0,\"module_family_collection_count\":0,\"repeated_body_clusters\":[]}", pyc.struct.collections = "[]"} {
    %oa, %ob, %oc, %od = pyc.comb(%a, %b, %c, %d) : (i8, i16, i64, i128) -> (i8, i16, i64, i128) {
    ^bb0(%aa: i8, %bb: i16, %cc: i64, %dd: i128):
      %s0_0 = pyc.add %aa, %aa : i8, i8 -> i8
      %s0_1 = pyc.add %bb, %bb : i16, i16 -> i16
      %s0_2 = pyc.add %cc, %cc : i64, i64 -> i64
      %s0_3 = pyc.add %dd, %dd : i128, i128 -> i128
      %s1_0 = pyc.xor %s0_0, %aa : i8, i8 -> i8
      %s1_1 = pyc.xor %s0_1, %bb : i16, i16 -> i16
      %s1_2 = pyc.xor %s0_2, %cc : i64, i64 -> i64
      %s1_3 = pyc.xor %s0_3, %dd : i128, i128 -> i128
      %s2_0 = pyc.add %s1_0, %aa : i8, i8 -> i8
      %s2_1 = pyc.add %s1_1, %bb : i16, i16 -> i16
      %s2_2 = pyc.add %s1_2, %cc : i64, i64 -> i64
      %s2_3 = pyc.add %s1_3, %dd : i128, i128 -> i128
      %s3_0 = pyc.xor %s2_0, %aa : i8, i8 -> i8
      %s3_1 = pyc.xor %s2_1, %bb : i16, i16 -> i16
      %s3_2 = pyc.xor %s2_2, %cc : i64, i64 -> i64
      %s3_3 = pyc.xor %s2_3, %dd : i128, i128 -> i128
      pyc.yield %s3_0, %s3_1, %s3_2, %s3_3 : i8, i16, i64, i128
    }
    return %oa, %ob, %oc, %od : i8, i16, i64, i128
  }
}
