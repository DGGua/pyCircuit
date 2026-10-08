#!/usr/bin/env python3
"""Scalar low-bit demand cases shared by the C++ and Verilog gates."""
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

cases = {
    "single_not": """
    %sum = pyc.add %a, %b : i32, i32 -> i32
    %inv = pyc.not %sum : i32
    %low = pyc.trunc %inv : i32 -> i4
    %y0 = pyc.zext %low : i4 -> i8
    return %y0, %zero12, %zero8, %zero8, %zero32 : i8, i12, i8, i8, i32
""",
    "shared_dag": """
    %sum = pyc.add %a, %b : i32, i32 -> i32
    %inv = pyc.not %sum : i32
    %masked = pyc.and %inv, %c : i32, i32 -> i32
    %branch = pyc.xor %masked, %d : i32, i32 -> i32
    %y0 = pyc.trunc %branch : i32 -> i8
    %y1 = pyc.extract %branch {lsb = 4 : i64, msb = 15 : i64} : i32 -> i12
    %alias = pyc.alias %branch : i32
    %product = pyc.mul %alias, %a : i32, i32 -> i32
    %inverted_product = pyc.not %product : i32
    %y2 = pyc.trunc %inverted_product : i32 -> i8
    %other = pyc.sub %sum, %d : i32, i32 -> i32
    %y3 = pyc.trunc %other : i32 -> i8
    return %y0, %y1, %y2, %y3, %zero32 : i8, i12, i8, i8, i32
""",
    "identical_prefix": """
    %bits = pyc.and %a, %b : i32, i32 -> i32
    %low = pyc.trunc %bits : i32 -> i8
    %slice = pyc.extract %bits {lsb = 0 : i64, msb = 7 : i64} : i32 -> i8
    %clow = pyc.trunc %c : i32 -> i8
    %dlow = pyc.trunc %d : i32 -> i8
    %y0 = pyc.add %low, %clow : i8, i8 -> i8
    %diff = pyc.sub %slice, %dlow : i8, i8 -> i8
    %y1 = pyc.zext %diff : i8 -> i12
    return %y0, %y1, %low, %slice, %zero32 : i8, i12, i8, i8, i32
""",
    "fullwidth_reader": """
    %sum = pyc.add %a, %b : i32, i32 -> i32
    %inv = pyc.not %sum : i32
    %y0 = pyc.trunc %inv : i32 -> i8
    %y1 = pyc.extract %inv {lsb = 8 : i64, msb = 19 : i64} : i32 -> i12
    return %y0, %y1, %zero8, %zero8, %inv : i8, i12, i8, i8, i32
""",
    "kept_reader": """
    %sum = pyc.add %a, %b : i32, i32 -> i32
    %inv = pyc.not %sum {pyc.debug_keep} : i32
    %y0 = pyc.trunc %inv : i32 -> i8
    return %y0, %zero12, %zero8, %zero8, %zero32 : i8, i12, i8, i8, i32
""",
}

for name, body in cases.items():
    attrs = ('arg_names = ["a", "b", "c", "d"], '
             'result_names = ["y0", "y1", "y2", "y3", "wide"], '
             'pyc.kind = "module", pyc.inline = "false", pyc.params = "{}", '
             'pyc.base = "top", pyc.struct.collections = "[]", '
             f"pyc.struct.metrics = {json.dumps(json.dumps(metrics))}")
    (out / f"{name}.mlir").write_text(
        'module attributes {pyc.top = @top, pyc.frontend.contract = "pycircuit"} {\n'
        '  func.func @top(%a: i32, %b: i32, %c: i32, %d: i32) -> '
        f'(i8, i12, i8, i8, i32) attributes {{{attrs}}} {{\n'
        '    %zero8 = pyc.constant 0 : i8\n'
        '    %zero12 = pyc.constant 0 : i12\n'
        '    %zero32 = pyc.constant 0 : i32\n' + body + '  }\n}\n')

for name in ("dynamic_left_single", "dynamic_left_shared", "dynamic_left_full"):
    attrs = ('arg_names = ["a", "amount"], '
             'result_names = ["low", "middle", "right", "wide"], '
             'pyc.kind = "module", pyc.inline = "false", pyc.params = "{}", '
             'pyc.base = "top", pyc.struct.collections = "[]", '
             f"pyc.struct.metrics = {json.dumps(json.dumps(metrics))}")
    middle = ("    %middle = pyc.extract %left {lsb = 8 : i64, msb = 15 : i64} : i128 -> i8\n"
              if name != "dynamic_left_single" else "    %middle = pyc.constant 0 : i8\n")
    wide = "%left" if name == "dynamic_left_full" else "%zero"
    (out / f"{name}.mlir").write_text(
        'module attributes {pyc.top = @top, pyc.frontend.contract = "pycircuit"} {\n'
        '  func.func @top(%a: i128, %amount: i8) -> (i4, i8, i4, i128) '
        f'attributes {{{attrs}}} {{\n'
        '    %zero = pyc.constant 0 : i128\n'
        '    %left = pyc.shl %a, %amount : i128, i8\n'
        '    %low = pyc.trunc %left : i128 -> i4\n' + middle +
        '    %right_shift = pyc.lshr %a, %amount : i128, i8\n'
        '    %right = pyc.trunc %right_shift : i128 -> i4\n'
        f'    return %low, %middle, %right, {wide} : i4, i8, i4, i128\n'
        '  }\n}\n')

for amount_width in (64, 128):
    attrs = ('arg_names = ["a", "amount"], '
             'result_names = ["left", "logical", "arithmetic", "low"], '
             'pyc.kind = "module", pyc.inline = "false", pyc.params = "{}", '
             'pyc.base = "top", pyc.struct.collections = "[]", '
             f"pyc.struct.metrics = {json.dumps(json.dumps(metrics))}")
    (out / f"wide_amount_{amount_width}.mlir").write_text(
        'module attributes {pyc.top = @top, pyc.frontend.contract = "pycircuit"} {\n'
        f'  func.func @top(%a: i128, %amount: i{amount_width}) -> (i128, i128, i128, i4) '
        f'attributes {{{attrs}}} {{\n'
        f'    %left = pyc.shl %a, %amount {{pyc.name = "kept_left", pyc.debug_keep}} : i128, i{amount_width}\n'
        f'    %logical = pyc.lshr %a, %amount : i128, i{amount_width}\n'
        f'    %arithmetic = pyc.ashr %a, %amount {{pyc.debug_keep}} : i128, i{amount_width}\n'
        '    %low = pyc.trunc %left : i128 -> i4\n'
        '    return %left, %logical, %arithmetic, %low : i128, i128, i128, i4\n'
        '  }\n}\n')
