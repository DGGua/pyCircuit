#!/usr/bin/env python3
"""Fixtures and structural checks for dependency-ready condition batching."""
import json
import re
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
KINDS = ("add", "xor", "or", "and", "sub", "mul")
SECOND = ("xor", "add", "sub", "mul", "or", "and")


def operation(result, kind, left, right):
    return f"    %{result} = pyc.{kind} %{left}, %{right} : i8, i8 -> i8"


def prepare(directory):
    directory.mkdir(parents=True, exist_ok=True)
    template = (HERE / "partition_cost.mlir").read_text()
    prefix = template[:template.index("    %n0")]
    prefix = prefix.replace("%a: i8, %b: i8, %c: i8",
                            "%sel: i1, %guard: i1, %a: i8, %b: i8, %c: i8")
    prefix = prefix.replace('arg_names = ["a", "b", "c"]',
                            'arg_names = ["sel", "guard", "a", "b", "c"]')
    prefix = prefix.replace("-> (i8, i8, i8, i8, i8, i8, i8, i8)",
                            "-> (i8, i8, i8, i8, i8, i8)")
    prefix = re.sub(r"result_names = \[[^]]*\]", "result_names = " +
                    json.dumps([f"out{i}" for i in range(6)]), prefix)
    for case in ("late", "late_tree", "select", "dependent", "effect"):
        ops = []
        if case == "effect":
            ops.append(operation("seed", "add", "a", "b"))
        for i, kind in enumerate(KINDS):
            left = "seed" if case == "effect" else "a"
            if case == "dependent":
                left = "a" if i == 0 else f"m{i-1}"
                kind = "add"
            ops.append(operation(f"t{i}", kind, left, "b"))
            arm = f"t{i}"
            if case == "late_tree":
                ops.append(operation(f"v{i}", SECOND[i], arm, "c"))
                arm = f"v{i}"
            if case == "select" and i % 2:
                ops.append(f"    %m{i} = arith.select %sel, %{arm}, %c : i8")
            else:
                ops.append(f"    %m{i} = pyc.mux %sel, %{arm}, %c : i1, i8, i8 -> i8")
            if case == "effect" and i == 2:
                ops.append('    pyc.assert %guard {msg = "shared-condition barrier"}')
        text = prefix + "\n".join(ops) + "\n    return " + ", ".join(
            f"%m{i}" for i in range(6)) + " : " + ", ".join(["i8"] * 6) + "\n  }\n}\n"
        (directory / f"{case}.mlir").write_text(text)


def check(directory):
    for case in ("late", "late_tree", "select", "dependent", "effect"):
        expected = 0 if case in ("dependent", "effect") else 1
        for mode in ("default", "bounded", "strict"):
            source = (directory / f"{case}_{mode}.cpp").read_text()
            actual = source.count("if (sel.toBool())")
            assert actual == expected, (case, mode, actual, expected)
    # Two-operation branch trees must be evaluated inside the selected arm,
    # rather than materialized unconditionally ahead of the common branch.
    source = (directory / "late_tree_default.cpp").read_text()
    branch = source.index("if (sel.toBool())")
    body = source[branch:source.index("} else {", branch)]
    assert " + " in body and " ^ " in body and " * " in body, body
    print("shared-condition oracle: late-ready, inline trees, select, dependency and effects passed")


if __name__ == "__main__":
    {"prepare": prepare, "check": check}[sys.argv[1]](Path(sys.argv[2]))
