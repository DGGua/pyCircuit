module attributes {pyc.top = @top, pyc.frontend.contract = "pycircuit"} {
  func.func @top(%a: i8, %b: i8, %c: i8) -> i8 attributes {arg_names = ["a", "b", "c"], result_names = ["out"], pyc.kind = "module", pyc.inline = "false", pyc.params = "{}", pyc.base = "top", pyc.struct.metrics = "{\"source_loc\":0,\"ast_node_count\":0,\"hardware_call_count\":0,\"loop_count\":0,\"module_call_count\":0,\"state_call_count\":0,\"estimated_inline_cost\":0,\"instance_count\":0,\"state_alloc_count\":0,\"collection_count\":0,\"collection_instance_count\":0,\"module_family_collection_count\":0,\"repeated_body_clusters\":[]}", pyc.struct.collections = "[]"} {
    %n0 = pyc.add %a, %b {pyc.name = "n0"} : i8, i8 -> i8
    %n1 = pyc.xor %n0, %c {pyc.name = "n1"} : i8, i8 -> i8
    %n2 = pyc.add %n1, %a {pyc.name = "n2"} : i8, i8 -> i8
    %n3 = pyc.xor %n2, %b {pyc.name = "n3"} : i8, i8 -> i8
    %n4 = pyc.add %n3, %c {pyc.name = "n4"} : i8, i8 -> i8
    %n5 = pyc.xor %n4, %a {pyc.name = "n5"} : i8, i8 -> i8
    %n6 = pyc.add %n5, %b {pyc.name = "n6"} : i8, i8 -> i8
    %n7 = pyc.xor %n6, %c {pyc.name = "n7"} : i8, i8 -> i8
    return %n7 : i8
  }
}
