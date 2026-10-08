module attributes {pyc.top = @top, pyc.frontend.contract = "pycircuit"} {
  func.func @top(%a: i16, %b: i16, %c: i16, %sel: i1) -> (i16, i16, i16, i16, i8, i8, i8) attributes {arg_names = ["a", "b", "c", "sel"], result_names = ["sum16", "sub16", "product16", "selected", "low8", "middle8", "crossing8"], pyc.kind = "module", pyc.inline = "false", pyc.params = "{}", pyc.base = "top", pyc.struct.metrics = "{\"source_loc\":0,\"ast_node_count\":0,\"hardware_call_count\":0,\"loop_count\":0,\"module_call_count\":0,\"state_call_count\":0,\"estimated_inline_cost\":0,\"instance_count\":0,\"state_alloc_count\":0,\"collection_count\":0,\"collection_instance_count\":0,\"module_family_collection_count\":0,\"repeated_body_clusters\":[]}", pyc.struct.collections = "[]"} {
    %a17 = pyc.zext %a : i16 -> i17
    %b17 = pyc.zext %b : i16 -> i17
    %add17 = pyc.add %a17, %b17 : i17, i17 -> i17
    %sub17 = pyc.sub %a17, %b17 : i17, i17 -> i17
    %sum16 = pyc.trunc %add17 : i17 -> i16
    %sub16 = pyc.trunc %sub17 : i17 -> i16
    %a32 = pyc.zext %a : i16 -> i32
    %b32 = pyc.zext %b : i16 -> i32
    %product32 = pyc.mul %a32, %b32 : i32, i32 -> i32
    %product16 = pyc.trunc %product32 : i32 -> i16
    %xor16 = pyc.xor %a, %c : i16, i16 -> i16
    %selected = pyc.mux %sel, %sum16, %xor16 : i1, i16, i16 -> i16
    %low8 = pyc.extract %selected {lsb = 0 : i64, msb = 7 : i64} : i16 -> i8
    %middle8 = pyc.extract %product32 {lsb = 8 : i64, msb = 15 : i64} : i32 -> i8
    %joined = pyc.concat (%a, %b) : (i16, i16) -> i32
    %crossing8 = pyc.extract %joined {lsb = 12 : i64, msb = 19 : i64} : i32 -> i8
    return %sum16, %sub16, %product16, %selected, %low8, %middle8, %crossing8 : i16, i16, i16, i16, i8, i8, i8
  }
}
