#!/usr/bin/env python3
"""Independent cut enumeration and structural checks for GSIM partitioning.

The small DAG oracle enumerates every legal cut set instead of reproducing the
compiler's dynamic-programming recurrence. Large fixtures exercise the source's
recipient-size boundaries without compiling a 7000-operation C++ testbench.
"""
import json
import re
import shutil
import sys
from pathlib import Path


HERE = Path(__file__).resolve().parent


def module(operations, outputs):
    template = (HERE / "partition_cost.mlir").read_text()
    prefix = template[:template.index("    %n0")]
    prefix = re.sub(r"-> \(i8, i8, i8, i8, i8, i8, i8, i8\)",
                    "-> (" + ", ".join("i8" for _ in outputs) + ")", prefix)
    prefix = re.sub(r"result_names = \[[^]]*\]",
                    "result_names = " + json.dumps(
                        [f"out{i}" for i in range(len(outputs))]), prefix)
    return (prefix + "\n".join(operations) + "\n    return " +
            ", ".join("%" + name for name in outputs) + " : " +
            ", ".join("i8" for _ in outputs) + "\n  }\n}\n")


def operation(name, left, right, kind="add"):
    return (f'    %{name} = pyc.{kind} %{left}, %{right} '
            f'{{pyc.name = "{name}"}} : i8, i8 -> i8')


def prepare(directory):
    directory.mkdir(parents=True, exist_ok=True)
    for name in ("atomic", "cost"):
        shutil.copyfile(HERE / f"partition_{name}.mlir", directory / f"{name}.mlir")
    ops = [operation(f"r{i}", "a", "b", kind)
           for i, kind in enumerate(("add", "xor", "or", "and"))]
    for i, pair in enumerate(((0, 3), (1, 2), (0, 3), (1, 2))):
        ops.append(operation(f"n{i}", f"r{pair[0]}", f"r{pair[1]}"))
    (directory / "sibling_hash.mlir").write_text(
        module(ops, [f"r{i}" for i in range(4)] + [f"n{i}" for i in range(4)]))
    for label, lengths in (("siblings31", [1] * 31),
                           ("siblings_overflow", [29, 3, 2])):
        ops = [operation("t", "a", "b"), operation("u", "a", "c", "xor")]
        outputs = ["t", "u"]
        index = 0
        for length in lengths:
            for part in range(length):
                left, right = ("t", "u") if not part else (f"n{index-1}", "a")
                ops.append(operation(f"n{index}", left, right))
                index += 1
            outputs.append(f"n{index-1}")
        (directory / f"{label}.mlir").write_text(module(ops, outputs))
    for count in (7001, 7002):
        ops = [operation("t", "a", "c", "xor"), operation("u", "b", "c", "or")]
        for index in range(count):
            left = "a" if index == 0 else f"n{index-1}"
            right = {0: "b", 1: "t", 2: "u"}.get(index, "a")
            ops.append(operation(f"n{index}", left, right))
        (directory / f"limit{count}.mlir").write_text(
            module(ops, ["t", "u", f"n{count-1}"]))


def emitted_partition(path, count):
    text = path.read_text()
    by_group = {}
    assigned = set()
    # Part helpers belong to the same supernode as their parent method.
    pattern = r"inline void eval_sim_group_(\d+)(?:_part_\d+)?\(\)\s*\{"
    for match in re.finditer(pattern, text):
        start = match.end()
        depth, end = 1, start
        while depth:
            depth += (text[end] == "{") - (text[end] == "}")
            end += 1
        members = {int(n) for n in re.findall(
            r"(?m)^\s*n(\d+) = ", text[start:end-1])}
        if members:
            assert not (members & assigned), (path, "duplicate evaluation", members & assigned)
            by_group.setdefault(match[1], set()).update(members)
            assigned.update(members)
    all_assigned = {int(n) for n in re.findall(r"(?m)^\s*n(\d+) = ", text)}
    assert all_assigned == set(range(count)), (path, "missing or extra expressions",
                                               set(range(count)) ^ all_assigned)
    groups = list(by_group.values()) + [{n} for n in all_assigned - assigned]
    return sorted(groups, key=min)


def enumerate_partitions(count, bound):
    for mask in range(1 << (count - 1)):
        cuts = [0] + [i + 1 for i in range(count - 1) if mask & (1 << i)] + [count]
        if all(end - begin <= bound for begin, end in zip(cuts, cuts[1:])):
            yield [set(range(begin, end)) for begin, end in zip(cuts, cuts[1:])]


def cut_cost(partition, edges):
    owners = {n: i for i, members in enumerate(partition) for n in members}
    return sum(owners[a] != owners[b] for a, b in edges)


def check(directory):
    expected = {
        "atomic": (8, [set(range(8))]),
        "atomic_strict": (8, [set(range(i, i + 2)) for i in range(0, 8, 2)]),
        "siblings31": (31, [set(range(30)), {30}]),
        "sibling_hash": (4, [{0, 2}, {1, 3}]),
        "siblings_overflow": (34, [set(range(32)), {32, 33}]),
        "limit7001": (7001, [set(range(7001))]),
        "limit7002": (7002, [{0}, set(range(1, 7002))]),
    }
    for name, (count, groups) in expected.items():
        actual = emitted_partition(directory / f"{name}.cpp", count)
        assert actual == groups, (name, "group sizes", [len(g) for g in actual],
                                   "expected", [len(g) for g in groups])
    parents = [(), (), (), (1, 2), (0, 2), (0, 4), (4, 5), (0, 3)]
    edges = {(parent, child) for child, prev in enumerate(parents) for parent in prev}
    candidates = list(enumerate_partitions(8, 3))
    optimum = min(cut_cost(candidate, edges) for candidate in candidates)
    optimal = [candidate for candidate in candidates if cut_cost(candidate, edges) == optimum]
    assert len(optimal) == 1 and optimum == 5
    greedy = [set(range(i, min(i + 3, 8))) for i in range(0, 8, 3)]
    assert cut_cost(greedy, edges) == 9
    actual = emitted_partition(directory / "cost.cpp", 8)
    assert actual == optimal[0], ("cost", actual, optimal)
    print("GSIM partition oracle: atomic units, optimal cuts and recipient bounds passed")


if __name__ == "__main__":
    {"prepare": prepare, "check": check}[sys.argv[1]](Path(sys.argv[2]))
