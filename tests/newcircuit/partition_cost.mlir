module attributes {pyc.top = @top, pyc.frontend.contract = "pycircuit"} {
  func.func @top(%a: i8, %b: i8, %c: i8) -> (i8, i8, i8, i8, i8, i8, i8, i8) attributes {arg_names = ["a", "b", "c"], result_names = ["out0", "out1", "out2", "out3", "out4", "out5", "out6", "out7"], pyc.kind = "module", pyc.inline = "false", pyc.params = "{}", pyc.base = "top", pyc.struct.metrics = "{\"source_loc\":0,\"ast_node_count\":0,\"hardware_call_count\":0,\"loop_count\":0,\"module_call_count\":0,\"state_call_count\":0,\"estimated_inline_cost\":0,\"instance_count\":0,\"state_alloc_count\":0,\"collection_count\":0,\"collection_instance_count\":0,\"module_family_collection_count\":0,\"repeated_body_clusters\":[]}", pyc.struct.collections = "[]"} {
    %n0 = pyc.add %a, %b {pyc.name = "n0"} : i8, i8 -> i8
    %n1 = pyc.xor %a, %c {pyc.name = "n1"} : i8, i8 -> i8
    %n2 = pyc.or %b, %c {pyc.name = "n2"} : i8, i8 -> i8
    %n3 = pyc.add %n1, %n2 {pyc.name = "n3"} : i8, i8 -> i8
    %n4 = pyc.xor %n0, %n2 {pyc.name = "n4"} : i8, i8 -> i8
    %n5 = pyc.or %n0, %n4 {pyc.name = "n5"} : i8, i8 -> i8
    %n6 = pyc.add %n4, %n5 {pyc.name = "n6"} : i8, i8 -> i8
    %n7 = pyc.xor %n0, %n3 {pyc.name = "n7"} : i8, i8 -> i8
    return %n0, %n1, %n2, %n3, %n4, %n5, %n6, %n7 : i8, i8, i8, i8, i8, i8, i8, i8
  }
}
