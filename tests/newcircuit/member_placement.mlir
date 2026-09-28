module attributes {pyc.top = @top, pyc.frontend.contract = "pycircuit"} {
  func.func @top(%a: i8, %b: i8, %c: i8, %sel: i1, %x: i64, %z: i64) -> (i8, i8, i128) attributes {arg_names = ["a", "b", "c", "sel", "x", "z"], result_names = ["out", "nested_out", "wide_out"], pyc.kind = "module", pyc.inline = "false", pyc.params = "{}", pyc.base = "top", pyc.struct.metrics = "{\"source_loc\":0,\"ast_node_count\":0,\"hardware_call_count\":0,\"loop_count\":0,\"module_call_count\":0,\"state_call_count\":0,\"estimated_inline_cost\":0,\"instance_count\":0,\"state_alloc_count\":0,\"collection_count\":0,\"collection_instance_count\":0,\"module_family_collection_count\":0,\"repeated_body_clusters\":[]}", pyc.struct.collections = "[]"} {

    %sum = pyc.add %a, %b : i8, i8 -> i8
    %mixed = pyc.xor %sum, %c : i8, i8 -> i8
    %probe = pyc.alias %mixed {pyc.name = "debug_named", pyc.debug_keep = true} : i8
    %choice = pyc.mux %sel, %probe, %b : i1, i8, i8 -> i8
    %inc = pyc.add %choice, %a : i8, i8 -> i8
    %result_mix = pyc.xor %inc, %sum : i8, i8 -> i8
    %result = pyc.mul %result_mix, %b : i8, i8 -> i8
    %nested = pyc.comb(%a, %b, %c) : (i8, i8, i8) -> i8 {
    ^bb0(%ca: i8, %cb: i8, %cc: i8):
      %inner = pyc.comb(%ca, %cb) : (i8, i8) -> i8 {
      ^bb1(%ia: i8, %ib: i8):
        %difference = pyc.sub %ia, %ib : i8, i8 -> i8
        %named = pyc.alias %difference {pyc.name = "debug_nested", pyc.debug_keep = true} : i8
        %inner_mix = pyc.xor %named, %ib : i8, i8 -> i8
        pyc.yield %inner_mix : i8
      }
      %product = pyc.mul %inner, %cc : i8, i8 -> i8
      %outer = pyc.add %product, %ca : i8, i8 -> i8
      pyc.yield %outer : i8
    }
    %wide = pyc.concat (%x, %z) : (i64, i64) -> i128
    %double = pyc.add %wide, %wide : i128, i128 -> i128
    %wide_probe = pyc.alias %double {pyc.name = "debug_wide", pyc.debug_keep = true} : i128
    %mask = pyc.constant 24197857203266734864629346612071973665 : i128
    %scramble = pyc.xor %wide_probe, %mask : i128, i128 -> i128
    %wide_result = pyc.add %scramble, %wide : i128, i128 -> i128
    return %result, %nested, %wide_result : i8, i8, i128
  }
}
