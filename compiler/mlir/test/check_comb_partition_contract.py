#!/usr/bin/env python3
"""Positive/negative gate for the hardened comb partition contract."""

from __future__ import annotations

import argparse
import json
import subprocess
import tempfile
from pathlib import Path


METRICS = {
    "ast_node_count": 0,
    "collection_count": 0,
    "collection_instance_count": 0,
    "estimated_inline_cost": 0,
    "hardware_call_count": 0,
    "instance_count": 0,
    "loop_count": 0,
    "module_call_count": 0,
    "module_family_collection_count": 0,
    "repeat_pressure": 0,
    "repeated_body_clusters": [],
    "source_loc": 0,
    "state_alloc_count": 0,
    "state_call_count": 0,
}


def function_attrs(name: str, partition_attrs: str = "") -> str:
    attrs = [
        'arg_names = ["a"]',
        f'pyc.base = "{name}"',
        'pyc.inline = "false"',
        'pyc.kind = "module"',
        'pyc.params = "{}"',
    ]
    if partition_attrs:
        attrs.append(partition_attrs)
    attrs.extend(
        [
            'pyc.struct.collections = "[]"',
            "pyc.struct.metrics = "
            + json.dumps(json.dumps(METRICS, separators=(",", ":"))),
            "pyc.value_param_types = []",
            "pyc.value_params = []",
            'result_names = ["y"]',
        ]
    )
    return ", ".join(attrs)


def comb(attrs: str, *, empty: bool = False) -> str:
    body = "      pyc.yield %arg1 : i8" if empty else (
        "      %1 = pyc.not %arg1 : i8\n"
        "      pyc.yield %1 : i8"
    )
    return f"""    %0 = pyc.comb(%arg0) {{{attrs}}} : (i8) -> i8 {{
    ^bb0(%arg1: i8):
{body}
    }}
    return %0 : i8"""


def stamped_attrs(
    *,
    work: int = 1,
    extra: str = "",
    parent: int = 0,
    part: int = 0,
    part_count: int = 1,
    max_nodes: int = 1,
) -> str:
    attrs = [
        f"pyc.partition.max_nodes = {max_nodes} : i64",
        f"pyc.partition.parent_id = {parent} : i64",
        f"pyc.partition.part_count = {part_count} : i64",
        f"pyc.partition.part_id = {part} : i64",
        'pyc.partition.plan_version = "gsim-unified-v2"',
        f"pyc.partition.work = {work} : i64",
    ]
    if extra:
        attrs.insert(0, extra)
    return ", ".join(attrs)


def function_partition_attrs(parts: int, work: int) -> str:
    return ", ".join(
        [
            f"pyc.partition.function_parts = {parts} : i64",
            'pyc.partition.function_plan = "gsim-unified-v2"',
            f"pyc.partition.function_work = {work} : i64",
        ]
    )


def module_text(name: str, function_partition: str, body: str) -> str:
    return f"""module attributes {{pyc.frontend.contract = "pycircuit", pyc.top = @{name}}} {{
  func.func @{name}(%arg0: i8) -> i8 attributes {{{function_attrs(name, function_partition)}}} {{
{body}
  }}
}}
"""


def two_comb_body(first_attrs: str, second_attrs: str) -> str:
    return f"""    %0 = pyc.comb(%arg0) {{{first_attrs}}} : (i8) -> i8 {{
    ^bb0(%arg1: i8):
      %1 = pyc.not %arg1 : i8
      pyc.yield %1 : i8
    }}
    %2 = pyc.comb(%0) {{{second_attrs}}} : (i8) -> i8 {{
    ^bb0(%arg1: i8):
      %3 = pyc.not %arg1 : i8
      pyc.yield %3 : i8
    }}
    return %2 : i8"""


def vector_module_text(
    name: str,
    *,
    input_type: str = "vector<2xi8>",
    output_type: str = "i8",
    input_name: str = "a",
    partition_result_name: str,
    index: int = 0,
) -> str:
    attrs = function_attrs(name, function_partition_attrs(1, 1)).replace(
        'arg_names = ["a"]', f'arg_names = ["{input_name}"]'
    )
    return f"""module attributes {{pyc.frontend.contract = "pycircuit", pyc.top = @{name}}} {{
  func.func @{name}(%arg0: {input_type}) -> {output_type} attributes {{{attrs}}} {{
    %0 = pyc.comb(%arg0) {{{stamped_attrs(extra=f'pyc.comb.result_names = ["{partition_result_name}"]')}}} : ({input_type}) -> {output_type} {{
    ^bb0(%arg1: {input_type}):
      %1 = pyc.v_get %arg1 [{index}] : {input_type} -> {output_type}
      pyc.yield %1 : {output_type}
    }}
    return %0 : {output_type}
  }}
}}
"""


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("pycc", type=Path)
    args = parser.parse_args()

    cases: list[tuple[str, str, tuple[str, ...]]] = []
    cases.append(
        (
            "partial_function_metadata",
            module_text(
                "partial_function_metadata",
                'pyc.partition.function_plan = "gsim-unified-v2"',
                "    return %arg0 : i8",
            ),
            ("must be present together",),
        )
    )
    cases.append(
        (
            "stamped_without_function_marker",
            module_text(
                "stamped_without_function_marker", "", comb(stamped_attrs())
            ),
            ("requires matching function partition metadata",),
        )
    )
    cases.append(
        (
            "unpartitioned_top_level_comb",
            module_text(
                "unpartitioned_top_level_comb",
                function_partition_attrs(0, 0),
                comb(""),
            ),
            ("unified function plan contains an unpartitioned pyc.comb",),
        )
    )
    cases.append(
        (
            "summary_mismatch",
            module_text(
                "summary_mismatch",
                function_partition_attrs(2, 2),
                comb(stamped_attrs()),
            ),
            (
                "function partition part-count metadata mismatch",
                "function partition work metadata mismatch",
            ),
        )
    )
    cases.append(
        (
            "zero_work_partition",
            module_text(
                "zero_work_partition",
                function_partition_attrs(1, 0),
                comb(stamped_attrs(work=0), empty=True),
            ),
            ("partition work must be positive",),
        )
    )
    cases.append(
        (
            "partition_integer_wrong_type",
            module_text(
                "partition_integer_wrong_type",
                function_partition_attrs(1, 1),
                comb(
                    stamped_attrs().replace(
                        "pyc.partition.work = 1 : i64",
                        "pyc.partition.work = 1 : i32",
                    )
                ),
            ),
            ("partition attribute 'pyc.partition.work' must be an i64 integer",),
        )
    )
    cases.append(
        (
            "result_names_not_array",
            module_text(
                "result_names_not_array",
                function_partition_attrs(1, 1),
                comb(stamped_attrs(extra='pyc.comb.result_names = "tap"')),
            ),
            ("'pyc.comb.result_names' must be an array of strings",),
        )
    )
    cases.append(
        (
            "result_names_wrong_arity",
            module_text(
                "result_names_wrong_arity",
                function_partition_attrs(1, 1),
                comb(
                    stamped_attrs(
                        extra='pyc.comb.result_names = ["tap", "extra"]'
                    )
                ),
            ),
            ("'pyc.comb.result_names' length must match result arity",),
        )
    )
    cases.append(
        (
            "result_names_wrong_entry",
            module_text(
                "result_names_wrong_entry",
                function_partition_attrs(1, 1),
                comb(stamped_attrs(extra="pyc.comb.result_names = [1 : i64]")),
            ),
            ("'pyc.comb.result_names' entry 0 must be a string",),
        )
    )
    cases.append(
        (
            "result_name_port_collision",
            module_text(
                "result_name_port_collision",
                function_partition_attrs(1, 1),
                comb(stamped_attrs(extra='pyc.comb.result_names = ["a"]')),
            ),
            ("duplicate canonical partition probe path 'a'",),
        )
    )
    cases.append(
        (
            "result_name_sanitized_port_collision",
            module_text(
                "result_name_sanitized_port_collision",
                function_partition_attrs(1, 1),
                comb(stamped_attrs(extra='pyc.comb.result_names = ["a.b"]')),
            ).replace('arg_names = ["a"]', 'arg_names = ["a_b"]'),
            ("partition probe path collision after backend sanitization",),
        )
    )
    cases.append(
        (
            "vector_lane_result_name_collision",
            vector_module_text(
                "vector_lane_result_name_collision",
                partition_result_name="a[0]",
            ),
            ("duplicate canonical partition probe path 'a[0]'",),
        )
    )
    cases.append(
        (
            "multidim_vector_lane_result_name_collision",
            vector_module_text(
                "multidim_vector_lane_result_name_collision",
                input_type="vector<2x3xi8>",
                output_type="vector<3xi8>",
                partition_result_name="a[1]",
                index=1,
            ),
            ("duplicate canonical partition probe path 'a[1][0]'",),
        )
    )
    cases.append(
        (
            "comb_wrapper_name_port_collision",
            module_text(
                "comb_wrapper_name_port_collision",
                function_partition_attrs(1, 1),
                comb(stamped_attrs(extra='pyc.name = "a"')),
            ),
            ("duplicate canonical partition probe path 'a'",),
        )
    )
    cases.append(
        (
            "unstamped_result_name_port_collision",
            module_text(
                "unstamped_result_name_port_collision",
                "",
                comb('pyc.comb.result_names = ["a"]'),
            ),
            ("duplicate canonical partition probe path 'a'",),
        )
    )
    cases.append(
        (
            "output_port_unrelated_named_value_collision",
            module_text(
                "output_port_unrelated_named_value_collision",
                "",
                """    %0 = pyc.alias %arg0 {pyc.name = \"y\"} : i8
    %1 = pyc.not %0 : i8
    return %1 : i8""",
            ),
            ("duplicate canonical partition probe path 'y'",),
        )
    )
    cases.append(
        (
            "inconsistent_function_max_nodes",
            module_text(
                "inconsistent_function_max_nodes",
                function_partition_attrs(2, 2),
                two_comb_body(
                    stamped_attrs(parent=0, max_nodes=1),
                    stamped_attrs(parent=1, max_nodes=2),
                ),
            ),
            ("all partitions in a unified function must use the same max_nodes",),
        )
    )

    valid_cases: list[tuple[str, str]] = [
        (
            "output_port_named_value_alias",
            module_text(
                "output_port_named_value_alias",
                "",
                """    %0 = pyc.alias %arg0 {pyc.name = \"y\"} : i8
    return %0 : i8""",
            ),
        ),
        (
            "comb_wrapper_result_name_same_owner",
            module_text(
                "comb_wrapper_result_name_same_owner",
                function_partition_attrs(1, 1),
                comb(
                    stamped_attrs(
                        extra='pyc.comb.result_names = ["tap"], pyc.name = "tap"'
                    )
                ),
            ),
        ),
        (
            "comb_wrapper_result_name_aliases",
            module_text(
                "comb_wrapper_result_name_aliases",
                function_partition_attrs(1, 1),
                comb(
                    stamped_attrs(
                        extra='pyc.comb.result_names = ["tap"], pyc.name = "tap_alias"'
                    )
                ),
            ),
        ),
        (
            "vector_lane_out_of_range",
            vector_module_text(
                "vector_lane_out_of_range",
                partition_result_name="a[2]",
            ),
        ),
        (
            "vector_leaf_not_sanitized_storage",
            vector_module_text(
                "vector_leaf_not_sanitized_storage",
                input_name="a.b",
                partition_result_name="a_b[0]",
            ),
        ),
    ]
    cases.append(
        (
            "sparse_parent_ids",
            module_text(
                "sparse_parent_ids",
                function_partition_attrs(2, 2),
                two_comb_body(
                    stamped_attrs(parent=0),
                    stamped_attrs(parent=2),
                ),
            ),
            ("partition parent_id values must be dense and ordered from zero",),
        )
    )
    cases.append(
        (
            "backward_cross_parent_dependency",
            module_text(
                "backward_cross_parent_dependency",
                function_partition_attrs(2, 2),
                two_comb_body(
                    stamped_attrs(parent=1),
                    stamped_attrs(parent=0),
                ),
            ),
            ("cross-parent partition dependency must advance parent_id",),
        )
    )

    common = [
        str(args.pycc),
        "--emit=none",
        "-o",
        "/dev/null",
        "--comb-update=dirty",
        "--comb-partition=none",
        "--logic-depth=256",
        "--build-profile=dev-fast",
        "--inline-policy=off",
        "--hierarchy-policy=strict",
    ]
    with tempfile.TemporaryDirectory(prefix="pyc-partition-contract-") as tmp:
        root = Path(tmp)
        for name, text, expected in cases:
            source = root / f"{name}.mlir"
            source.write_text(text, encoding="utf-8")
            proc = subprocess.run(
                [common[0], str(source), *common[1:]],
                text=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                check=False,
            )
            if proc.returncode == 0:
                raise SystemExit(f"fail: malformed case {name} passed pycc")
            for diagnostic in expected:
                if diagnostic not in proc.stdout:
                    raise SystemExit(
                        f"fail: case {name} missed diagnostic {diagnostic!r}\n"
                        + proc.stdout
                    )

        for name, text in valid_cases:
            source = root / f"{name}.mlir"
            source.write_text(text, encoding="utf-8")
            proc = subprocess.run(
                [common[0], str(source), *common[1:]],
                text=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                check=False,
            )
            if proc.returncode != 0:
                raise SystemExit(f"fail: valid case {name} failed pycc\n" + proc.stdout)

    print(
        f"ok: rejected {len(cases)} malformed cases; "
        f"accepted {len(valid_cases)} namespace cases"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
