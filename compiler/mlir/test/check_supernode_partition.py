#!/usr/bin/env python3
"""Structural checks for the pyc-partition-comb gate dump."""

from __future__ import annotations

import argparse
import re
import sys
from collections import defaultdict
from pathlib import Path


def fail(message: str) -> None:
    raise SystemExit(f"fail: {message}")


def attr_value(line: str, name: str) -> int | None:
    match = re.search(rf'{re.escape(name)}"?\s*=\s*(-?\d+)', line)
    return int(match.group(1)) if match else None


def attr_string(line: str, name: str) -> str | None:
    match = re.search(rf'{re.escape(name)}"?\s*=\s*"([^"]+)"', line)
    return match.group(1) if match else None


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("dump_dir", type=Path)
    parser.add_argument("--max-nodes", type=int, default=1)
    parser.add_argument("--require-grouped", action="store_true")
    parser.add_argument(
        "--expected-work",
        default="",
        help="Optional comma-separated work sequence for the single parent plan",
    )
    args = parser.parse_args()
    if args.max_nodes <= 0:
        fail("--max-nodes must be positive")
    dump_dir = args.dump_dir
    candidates = sorted(dump_dir.glob("*_after_*partition-comb*.mlir"))
    if not candidates:
        fail(f"no after dump for pyc-partition-comb under {dump_dir}")

    # The pass is func-nested, so a multi-module input may yield one file per
    # function.  This fixture has one function; concatenate defensively.
    text = "\n".join(path.read_text(encoding="utf-8") for path in candidates)
    comb_lines = [line for line in text.splitlines() if "pyc.comb(" in line]
    if len(comb_lines) < 8:
        fail(f"expected sibling comb partitions, found only {len(comb_lines)}")

    required = (
        "pyc.partition.parent_id",
        "pyc.partition.part_id",
        "pyc.partition.part_count",
        "pyc.partition.plan_version",
        "pyc.partition.work",
        "pyc.partition.max_nodes",
    )
    for attr in required:
        if text.count(attr) < len(comb_lines):
            fail(f"partition metadata {attr!r} is absent from one or more combs")

    by_parent: dict[int, list[int]] = defaultdict(list)
    work_by_parent: dict[int, dict[int, int]] = defaultdict(dict)
    declared_counts: dict[int, set[int]] = defaultdict(set)
    declared_max_nodes: dict[int, set[int]] = defaultdict(set)
    for line in comb_lines:
        parent = attr_value(line, "pyc.partition.parent_id")
        part = attr_value(line, "pyc.partition.part_id")
        part_count = attr_value(line, "pyc.partition.part_count")
        work = attr_value(line, "pyc.partition.work")
        max_nodes = attr_value(line, "pyc.partition.max_nodes")
        version = attr_string(line, "pyc.partition.plan_version")
        if (
            parent is None
            or part is None
            or part_count is None
            or work is None
            or max_nodes is None
            or version is None
        ):
            fail(f"could not parse complete partition metadata: {line.strip()}")
        if work <= 0 or work > args.max_nodes:
            fail(
                f"--comb-partition-max-nodes={args.max_nodes} produced "
                f"invalid work={work}"
            )
        if max_nodes != args.max_nodes:
            fail(
                f"partition max_nodes metadata is {max_nodes}, "
                f"expected {args.max_nodes}"
            )
        if part_count <= 0 or part >= part_count:
            fail(f"invalid part_id/part_count pair: {part}/{part_count}")
        if version != "gsim-contiguous-dp-v1":
            fail(f"unexpected partition plan_version: {version!r}")
        by_parent[parent].append(part)
        work_by_parent[parent][part] = work
        declared_counts[parent].add(part_count)
        declared_max_nodes[parent].add(max_nodes)

    for parent, parts in by_parent.items():
        ordered = sorted(parts)
        if ordered != list(range(len(ordered))):
            fail(f"parent {parent} part ids are not dense 0..N-1: {ordered}")
        if declared_counts[parent] != {len(parts)}:
            fail(
                f"parent {parent} part_count disagrees with siblings: "
                f"{declared_counts[parent]} vs {len(parts)}"
            )
        if declared_max_nodes[parent] != {args.max_nodes}:
            fail(f"parent {parent} max_nodes disagrees across siblings")

    if args.require_grouped and not any(
        work > 1 for work_map in work_by_parent.values() for work in work_map.values()
    ):
        fail("partition plan did not coarsen/group any operations")
    if args.expected_work:
        if len(work_by_parent) != 1:
            fail("--expected-work requires exactly one parent plan")
        expected = [int(item) for item in args.expected_work.split(",") if item]
        work_map = next(iter(work_by_parent.values()))
        actual = [work_map[part] for part in sorted(work_map)]
        if actual != expected:
            fail(f"partition work sequence changed: expected {expected}, got {actual}")

    # This catches accidental scalar-only lowering and loss of the constant
    # partition.  The latter has no operands and must still execute once.
    if "vector<4xi8>" not in text:
        fail("vector-valued partition boundary disappeared")
    if "i130" not in text:
        fail("wide partition boundary disappeared")
    if "!pyc.reset" not in text or "pyc.reset_active" not in text:
        fail("reset_active/!pyc.reset memoizable partition disappeared")
    if args.max_nodes == 1 and not any(
        re.search(r"pyc\.comb\(\)\s*\{", line) for line in comb_lines
    ):
        fail("expected a zero-input constant partition")

    print(
        f"ok: {len(comb_lines)} sibling comb partitions across "
        f"{len(by_parent)} parent plan(s)"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
