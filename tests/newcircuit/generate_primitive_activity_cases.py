#!/usr/bin/env python3
"""Produce state-boundary activity cases without changing primitive semantics."""
import json
import pathlib
import sys

out = pathlib.Path(sys.argv[1])
metrics = dict.fromkeys([
    "source_loc", "ast_node_count", "hardware_call_count", "loop_count",
    "module_call_count", "state_call_count", "estimated_inline_cost",
    "instance_count", "state_alloc_count", "collection_count",
    "collection_instance_count", "module_family_collection_count"], 0)
metrics["repeated_body_clusters"] = []


def emit(name, args, results, body):
    arg_text = ", ".join(f"%{key}: {typ}" for key, typ in args)
    result_text = ", ".join(typ for _, typ in results)
    attrs = (f"arg_names = {json.dumps([key for key, _ in args])}, "
             f"result_names = {json.dumps([key for key, _ in results])}, "
             'pyc.kind = "module", pyc.inline = "false", pyc.params = "{}", '
             'pyc.base = "top", pyc.struct.collections = "[]", '
             f"pyc.struct.metrics = {json.dumps(json.dumps(metrics))}")
    (out / f"{name}.mlir").write_text(
        'module attributes {pyc.top = @top, pyc.frontend.contract = "pycircuit"} {\n'
        f"  func.func @top({arg_text}) -> ({result_text}) attributes {{{attrs}}} {{\n"
        + body + "\n  }\n}\n")


for name in ("fifo", "async_fifo"):
    clocks = ([("clk", "!pyc.clock"), ("rst", "!pyc.reset")] if name == "fifo" else
              [("in_clk", "!pyc.clock"), ("in_rst", "!pyc.reset"),
               ("out_clk", "!pyc.clock"), ("out_rst", "!pyc.reset")])
    args = clocks + [("in_valid", "i1"), ("in_data", "i8"), ("out_ready", "i1")]
    operands = ", ".join(f"%{key}" for key, _ in args)
    emit(name, args, [("in_ready", "i1"), ("out_valid", "i1"), ("out_data", "i8"),
                      ("y", "i8"), ("status", "i1")],
         f"    %ready, %valid, %data = pyc.{name} {operands} {{depth = 4 : i64}} : i8\n"
         "    %sum = pyc.add %data, %data : i8, i8 -> i8\n"
         "    %y = pyc.xor %sum, %data : i8, i8 -> i8\n"
         "    %status = pyc.xor %ready, %valid : i1, i1 -> i1\n"
         "    return %ready, %valid, %data, %y, %status : i1, i1, i8, i8, i1")

args = [("clk", "!pyc.clock"), ("rst", "!pyc.reset"), ("ren0", "i1"),
        ("raddr0", "i3"), ("ren1", "i1"), ("raddr1", "i3"), ("wvalid", "i1"),
        ("waddr", "i3"), ("wdata", "i8"), ("wstrb", "i1")]
emit("sync_mem_dp", args, [("q0", "i8"), ("q1", "i8"), ("y", "i8")],
     "    %q0, %q1 = pyc.sync_mem_dp " + ", ".join(f"%{key}" for key, _ in args) +
     ' {depth = 4 : i64, name = "activity_mem"} : i3, i8, i1\n'
     "    %sum = pyc.add %q0, %q1 : i8, i8 -> i8\n"
     "    %y = pyc.xor %sum, %q0 : i8, i8 -> i8\n"
     "    return %q0, %q1, %y : i8, i8, i8")

emit("cdc", [("clk", "!pyc.clock"), ("rst", "!pyc.reset"), ("d", "i8")],
     [("q", "i8"), ("y", "i8")],
     "    %q = pyc.cdc_sync %clk, %rst, %d {stages = 3 : i64} : i8\n"
     "    %sum = pyc.add %q, %q : i8, i8 -> i8\n"
     "    %y = pyc.xor %sum, %q : i8, i8 -> i8\n"
     "    return %q, %y : i8, i8")
