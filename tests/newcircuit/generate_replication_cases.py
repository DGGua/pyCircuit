#!/usr/bin/env python3
"""Generate legal PYC expression DAGs for the standalone replication gate."""
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


barrier = """    %barrier = pyc.comb(%d) : (i8) -> i8 {
      ^bb0(%d0: i8):
        %inv = pyc.not %d0 : i8
        pyc.yield %inv : i8
    }
"""
args = [(key, "i8") for key in ("a", "b", "c", "d")]
for name, rhs, attr in [("compound", "%seed, %c", ""),
                        ("repeated", "%seed, %seed", ""),
                        ("named", "%seed, %c", ' {pyc.name = "anchor"}')]:
    emit(name, args, [("y", "i8")],
         "    %seed = pyc.xor %a, %b : i8, i8 -> i8\n"
         f"    %mid = pyc.add {rhs}{attr} : i8, i8 -> i8\n" + barrier +
         "    %mixed = pyc.xor %mid, %barrier : i8, i8 -> i8\n"
         "    %y = pyc.add %mixed, %d : i8, i8 -> i8\n"
         "    return %y : i8")

emit("fanout", args, [("y", "i8"), ("z", "i8")],
     "    %seed = pyc.xor %a, %b : i8, i8 -> i8\n"
     "    %mid = pyc.add %seed, %c : i8, i8 -> i8\n" + barrier +
     "    %y = pyc.xor %mid, %barrier : i8, i8 -> i8\n"
     "    %z = pyc.sub %mid, %barrier : i8, i8 -> i8\n"
     "    return %y, %z : i8, i8")

emit("wide_input", [("a", "i128"), ("d", "i8")], [("y", "i8")],
     "    %picked = pyc.extract %a {lsb = 60 : i64, msb = 67 : i64} : i128 -> i8\n"
     + barrier + "    %y = pyc.xor %picked, %barrier : i8, i8 -> i8\n"
     "    return %y : i8")

emit("array_compound", [("v", "vector<2xi8>"), ("c", "i8"), ("d", "i8")],
     [("y", "i8")],
     "    %picked = pyc.v_get %v[1] : vector<2xi8> -> i8\n"
     "    %mid = pyc.add %picked, %c : i8, i8 -> i8\n" + barrier +
     "    %mixed = pyc.xor %mid, %barrier : i8, i8 -> i8\n"
     "    %y = pyc.add %mixed, %d : i8, i8 -> i8\n"
     "    return %y : i8")

emit("array_row", [("v", "vector<2x2xi8>"), ("d", "i8")], [("y", "i8")],
     "    %row = pyc.v_get %v[1] : vector<2x2xi8> -> vector<2xi8>\n"
     "    %picked = pyc.v_get %row[1] : vector<2xi8> -> i8\n" + barrier +
     "    %y = pyc.xor %picked, %barrier : i8, i8 -> i8\n"
     "    return %y : i8")

emit("wide_result", [("a", "i128"), ("b", "i128"), ("d", "i128")],
     [("y", "i128")],
     "    %seed = pyc.xor %a, %b : i128, i128 -> i128\n"
     + barrier.replace("i8", "i128") +
     "    %y = pyc.xor %seed, %barrier : i128, i128 -> i128\n"
     "    return %y : i128")

emit("state", [("clk", "!pyc.clock"), ("rst", "!pyc.reset"), ("en", "i1"),
               ("next", "i8"), ("init", "i8"), ("a", "i8"), ("c", "i8"),
               ("d", "i8")], [("q", "i8"), ("y", "i8")],
     "    %state = pyc.reg %clk, %rst, %en, %next, %init : i8\n"
     "    %seed = pyc.xor %state, %a : i8, i8 -> i8\n"
     "    %mid = pyc.add %seed, %c : i8, i8 -> i8\n" + barrier +
     "    %mixed = pyc.xor %mid, %barrier : i8, i8 -> i8\n"
     "    %y = pyc.add %mixed, %d : i8, i8 -> i8\n"
     "    return %state, %y : i8, i8")

for name, op, extra_args in [
        ("mux_compound", "pyc.mux %sel, %a, %b : i1, i8, i8 -> i8", [("sel", "i1")]),
        ("div_compound", "pyc.udiv %a, %b : i8, i8 -> i8", [])]:
    emit(name, args + extra_args, [("y", "i8")],
         f"    %seed = {op}\n"
         "    %mid = pyc.add %seed, %c : i8, i8 -> i8\n" + barrier +
         "    %mixed = pyc.xor %mid, %barrier : i8, i8 -> i8\n"
         "    %y = pyc.add %mixed, %d : i8, i8 -> i8\n"
         "    return %y : i8")
