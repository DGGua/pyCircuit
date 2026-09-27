module attributes {pyc.top = @top, pyc.frontend.contract = "pycircuit"} {
  func.func @top(%clk: !pyc.clock, %rst: !pyc.reset, %ren0: i1, %ren1: i1, %raddr0: i2, %raddr1: i2, %wvalid: i1, %waddr: i2, %wdata: i32, %wstrb: i4) -> (i8, i8, i8) attributes {arg_names = ["clk", "rst", "ren0", "ren1", "raddr0", "raddr1", "wvalid", "waddr", "wdata", "wstrb"], result_names = ["low16", "low32a", "low32b"], pyc.kind = "module", pyc.inline = "false", pyc.params = "{}", pyc.base = "top", pyc.struct.metrics = "{\"source_loc\":0,\"ast_node_count\":0,\"hardware_call_count\":0,\"loop_count\":0,\"module_call_count\":0,\"state_call_count\":1,\"estimated_inline_cost\":0,\"instance_count\":0,\"state_alloc_count\":1,\"collection_count\":0,\"collection_instance_count\":0,\"module_family_collection_count\":0,\"repeated_body_clusters\":[]}", pyc.struct.collections = "[]"} {
    %data16 = pyc.trunc %wdata : i32 -> i16
    %strb16 = pyc.trunc %wstrb : i4 -> i2
    %read16 = pyc.sync_mem %clk, %rst, %ren0, %raddr0, %wvalid, %waddr, %data16, %strb16 {depth = 4 : i64, name = "mem16"} : i2, i16, i2
    %read32a, %read32b = pyc.sync_mem_dp %clk, %rst, %ren0, %raddr0, %ren1, %raddr1, %wvalid, %waddr, %wdata, %wstrb {depth = 4 : i64, name = "mem32"} : i2, i32, i4
    %low16 = pyc.trunc %read16 : i16 -> i8
    %low32a = pyc.trunc %read32a : i32 -> i8
    %low32b = pyc.trunc %read32b : i32 -> i8
    return %low16, %low32a, %low32b : i8, i8, i8
  }
}
