module attributes {pyc.top = @top, pyc.frontend.contract = "pycircuit"} {
  func.func @top(%clk: !pyc.clock, %rst: !pyc.reset, %en: i1, %a: i8, %b: i8) -> i8 attributes {arg_names = ["clk", "rst", "en", "a", "b"], result_names = ["y"], pyc.kind = "module", pyc.inline = "false", pyc.params = "{}", pyc.base = "top", pyc.struct.metrics = "{\"source_loc\":0,\"ast_node_count\":0,\"hardware_call_count\":0,\"loop_count\":0,\"module_call_count\":0,\"state_call_count\":1,\"estimated_inline_cost\":0,\"instance_count\":0,\"state_alloc_count\":1,\"collection_count\":0,\"collection_instance_count\":0,\"module_family_collection_count\":0,\"repeated_body_clusters\":[]}", pyc.struct.collections = "[]"} {
    %zero = pyc.constant 0 : i8
    %dead = pyc.add %a, %b {pyc.name = "unused_name_hint"} : i8, i8 -> i8
    %deadq = pyc.reg %clk, %rst, %en, %a, %zero {pyc.name = "unused_state_hint"} : i8
    %kept = pyc.add %a, %b {pyc.name = "kept_sum", pyc.debug_keep = true} : i8, i8 -> i8
    %same = pyc.add %a, %b {pyc.name = "kept_duplicate", pyc.debug_keep = true} : i8, i8 -> i8
    %folded = pyc.add %a, %zero {pyc.name = "kept_identity", pyc.debug_keep = true} : i8, i8 -> i8
    %keptq = pyc.reg %clk, %rst, %en, %a, %zero {pyc.name = "kept_state", pyc.debug_keep = true} : i8
    %bit0 = pyc.extract %a {lsb = 0 : i64, msb = 0 : i64} : i8 -> i1
    %bit1 = pyc.extract %b {lsb = 0 : i64, msb = 0 : i64} : i8 -> i1
    %zero1 = pyc.constant 0 : i1
    %flag0 = pyc.reg %clk, %rst, %en, %bit0, %zero1 {pyc.name = "kept_flag0", pyc.debug_keep = true} : i1
    %flag1 = pyc.reg %clk, %rst, %en, %bit1, %zero1 {pyc.name = "kept_flag1", pyc.debug_keep = true} : i1
    %aliasq0 = pyc.reg %clk, %rst, %en, %bit0, %zero1 : i1
    %aliasf0 = pyc.alias %aliasq0 {pyc.name = "kept_alias_flag0", pyc.debug_keep = true} : i1
    %aliasq1 = pyc.reg %clk, %rst, %en, %bit1, %zero1 : i1
    %aliasf1 = pyc.alias %aliasq1 {pyc.name = "kept_alias_flag1", pyc.debug_keep = true} : i1
    %vector_d = pyc.v_create (%a, %b) : (i8, i8) -> vector<2xi8>
    %vector_init = pyc.v_broadcast %zero to 2 : i8 -> vector<2xi8>
    %vector_state = pyc.reg %clk, %rst, %en, %vector_d, %vector_init {pyc.name = "kept_vector", pyc.debug_keep = true} : vector<2xi8>
    %w = pyc.wire {pyc.name = "kept_wire", pyc.debug_keep = true} : i8
    pyc.assign %w, %a : i8
    %disabled = pyc.add %a, %b {pyc.name = "disabled_keep", pyc.debug_keep = false} : i8, i8 -> i8
    %nested = pyc.comb(%a, %b) : (i8, i8) -> i8 {
    ^bb0(%arg0: i8, %arg1: i8):
      %observed = pyc.xor %arg0, %arg1 {pyc.name = "nested_keep", pyc.debug_keep = true} : i8, i8 -> i8
      pyc.yield %arg0 : i8
    }
    return %b : i8
  }
}
