#!/usr/bin/env python3
"""Collect reproducible perf stat counters for every paired benchmark.

The throughput experiment must have been built first by run_experiment.py.  For
each case this script chooses one shared logical-cycle count for both backends,
then collects core and cache events in separate perf invocations so the events
fit in hardware counters without multiplexing.
"""

from __future__ import annotations

import argparse
import csv
import datetime as dt
import json
import math
import os
import platform
import shutil
import subprocess
from pathlib import Path
from typing import Iterable


CORE_EVENTS = ("cycles:u", "instructions:u", "branches:u", "branch-misses:u")
CACHE_EVENTS = (
    "L1-dcache-loads:u",
    "L1-dcache-load-misses:u",
    "LLC-loads:u",
    "LLC-load-misses:u",
)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--run-root",
        type=Path,
        default=Path(".pycircuit_out/perf-cpp-vs-verilator-20260811"),
        help="root produced by run_experiment.py",
    )
    parser.add_argument(
        "--throughput-json",
        type=Path,
        help="throughput results JSON (default: RUN_ROOT/results/results.json)",
    )
    parser.add_argument("--cpu", type=int, default=0)
    parser.add_argument("--repetitions", type=int, default=3)
    parser.add_argument(
        "--target-seconds",
        type=float,
        default=0.12,
        help="minimum estimated runtime of the faster backend per repetition",
    )
    parser.add_argument("--sample-every", type=int, default=4096)
    parser.add_argument(
        "--max-core-cycles-stddev",
        type=float,
        default=3.0,
        help="retry a core group when repeated hardware cycles vary more than this percent",
    )
    parser.add_argument(
        "--stability-retries",
        type=int,
        default=3,
        help="maximum retries for an unstable core group",
    )
    parser.add_argument(
        "--publish-csv",
        type=Path,
        help="optionally copy the normalized final CSV to a report-visible path",
    )
    parser.add_argument(
        "--force",
        action="store_true",
        help="discard resumable raw perf output and collect it again",
    )
    return parser.parse_args()


def checked_output(command: list[str]) -> str:
    return subprocess.check_output(command, text=True).strip()


def round_cycles(cycles: float) -> int:
    return max(100_000, int(math.ceil(cycles / 10_000.0)) * 10_000)


def event_key(event: str) -> str:
    return event.removesuffix(":u")


def parse_perf_csv(text: str, expected_events: Iterable[str]) -> dict[str, dict[str, float]]:
    parsed: dict[str, dict[str, float]] = {}
    for row in csv.reader(text.splitlines()):
        if len(row) < 3 or not row[0] or row[0].startswith("#"):
            continue
        key = event_key(row[2].strip())
        if key not in {event_key(item) for item in expected_events}:
            continue
        if row[0].startswith("<"):
            raise RuntimeError(f"perf could not count {key}: {row[0]}")
        coverage = float(row[5]) if len(row) > 5 and row[5] else 100.0
        stddev = float(row[3].removesuffix("%")) if len(row) > 3 and row[3] else 0.0
        parsed[key] = {
            "count": float(row[0]),
            "coverage_percent": coverage,
            "stddev_percent": stddev,
        }
    missing = {event_key(item) for item in expected_events} - parsed.keys()
    if missing:
        raise RuntimeError(f"missing perf events: {sorted(missing)}")
    return parsed


def run_perf(
    *,
    executable: Path,
    cycles: int,
    sample_every: int,
    cpu: int,
    repetitions: int,
    events: tuple[str, ...],
    raw_base: Path,
    force: bool,
) -> dict[str, dict[str, float]]:
    perf_path = raw_base.with_suffix(".perf.csv")
    stdout_path = raw_base.with_suffix(".stdout.txt")
    if perf_path.exists() and stdout_path.exists() and not force:
        completed_runs = [
            line
            for line in stdout_path.read_text().splitlines()
            if line.startswith("backend=")
        ]
        expected_cycle_token = f"cycles={cycles}"
        if len(completed_runs) == repetitions and all(
            expected_cycle_token in line.split() for line in completed_runs
        ):
            return parse_perf_csv(perf_path.read_text(), events)

    command = [
        "perf",
        "stat",
        "-x,",
        "-r",
        str(repetitions),
        "-e",
        ",".join(events),
        "--",
        "taskset",
        "-c",
        str(cpu),
        str(executable),
        str(cycles),
        str(sample_every),
    ]
    env = os.environ.copy()
    env["LC_ALL"] = "C"
    completed = subprocess.run(command, capture_output=True, text=True, env=env)
    stdout_path.write_text(completed.stdout)
    perf_path.write_text(completed.stderr)
    if completed.returncode != 0:
        raise RuntimeError(
            f"perf failed ({completed.returncode}): {' '.join(command)}\n{completed.stderr}"
        )
    return parse_perf_csv(completed.stderr, events)


def ratio(numerator: float, denominator: float) -> float:
    return numerator / denominator if denominator else 0.0


def summarize(
    *,
    case: str,
    backend: str,
    logical_cycles: int,
    repetitions: int,
    baseline_cps: float,
    core: dict[str, dict[str, float]],
    cache: dict[str, dict[str, float]],
) -> dict[str, float | int | str]:
    hw_cycles = core["cycles"]["count"]
    instructions = core["instructions"]["count"]
    branches = core["branches"]["count"]
    branch_misses = core["branch-misses"]["count"]
    l1_loads = cache["L1-dcache-loads"]["count"]
    l1_misses = cache["L1-dcache-load-misses"]["count"]
    llc_loads = cache["LLC-loads"]["count"]
    llc_misses = cache["LLC-load-misses"]["count"]
    return {
        "case": case,
        "backend": backend,
        "logical_cycles": logical_cycles,
        "repetitions": repetitions,
        "baseline_cycles_per_second": baseline_cps,
        "hardware_cycles_per_logical_cycle": ratio(hw_cycles, logical_cycles),
        "instructions_per_logical_cycle": ratio(instructions, logical_cycles),
        "ipc": ratio(instructions, hw_cycles),
        "branches_per_logical_cycle": ratio(branches, logical_cycles),
        "branch_miss_percent": ratio(branch_misses, branches) * 100.0,
        "l1d_loads_per_logical_cycle": ratio(l1_loads, logical_cycles),
        "l1d_miss_percent": ratio(l1_misses, l1_loads) * 100.0,
        "llc_loads_per_logical_cycle": ratio(llc_loads, logical_cycles),
        "llc_miss_percent": ratio(llc_misses, llc_loads) * 100.0,
        "core_min_coverage_percent": min(item["coverage_percent"] for item in core.values()),
        "cache_min_coverage_percent": min(item["coverage_percent"] for item in cache.values()),
        "hardware_cycles_stddev_percent": core["cycles"]["stddev_percent"],
        "instructions_stddev_percent": core["instructions"]["stddev_percent"],
        "core_max_stddev_percent": max(item["stddev_percent"] for item in core.values()),
        "cache_max_stddev_percent": max(item["stddev_percent"] for item in cache.values()),
    }


def write_results(output_root: Path, environment: dict[str, object], rows: list[dict[str, object]]) -> None:
    output_root.mkdir(parents=True, exist_ok=True)
    (output_root / "results.json").write_text(
        json.dumps({"environment": environment, "results": rows}, indent=2) + "\n"
    )
    columns = list(rows[0])
    with (output_root / "results.csv").open("w", newline="") as stream:
        writer = csv.DictWriter(stream, fieldnames=columns)
        writer.writeheader()
        writer.writerows(rows)


def main() -> int:
    args = parse_args()
    if args.repetitions < 1 or args.target_seconds <= 0:
        raise SystemExit("repetitions and target-seconds must be positive")
    if args.stability_retries < 0 or args.max_core_cycles_stddev <= 0:
        raise SystemExit("stability-retries must be nonnegative and max stddev must be positive")
    if args.sample_every < 1 or args.sample_every & (args.sample_every - 1):
        raise SystemExit("sample-every must be a positive power of two")
    if shutil.which("perf") is None or shutil.which("taskset") is None:
        raise SystemExit("perf and taskset are required")

    run_root = args.run_root.resolve()
    throughput_json = (args.throughput_json or run_root / "results/results.json").resolve()
    source = json.loads(throughput_json.read_text())
    cases = source["results"]
    output_root = run_root / "results/perf-stat-all"
    raw_root = output_root / "raw"
    raw_root.mkdir(parents=True, exist_ok=True)

    rows: list[dict[str, object]] = []
    for case_index, item in enumerate(cases):
        case = item["key"]
        shared_cycles = round_cycles(
            max(item["cpp_cps_median"], item["verilator_cps_median"]) * args.target_seconds
        )
        backend_order = ("cpp", "verilator") if case_index % 2 == 0 else ("verilator", "cpp")
        case_results: dict[str, dict[str, dict[str, float]]] = {}
        for backend in backend_order:
            executable = (
                run_root / case / "bench/bench_cpp"
                if backend == "cpp"
                else run_root / case / "bench/verilator_obj/bench_verilator"
            )
            if not executable.is_file():
                raise RuntimeError(f"missing executable: {executable}")
            slug = case.replace("/", "__")
            group_order = (
                (("core", CORE_EVENTS), ("cache", CACHE_EVENTS))
                if (case_index + (backend == "verilator")) % 2 == 0
                else (("cache", CACHE_EVENTS), ("core", CORE_EVENTS))
            )
            collected: dict[str, dict[str, dict[str, float]]] = {}
            for group_name, events in group_order:
                print(
                    f"[{case_index + 1:02d}/{len(cases)}] {case} {backend} {group_name} "
                    f"cycles={shared_cycles}",
                    flush=True,
                )
                for attempt in range(args.stability_retries + 1):
                    result = run_perf(
                        executable=executable,
                        cycles=shared_cycles,
                        sample_every=args.sample_every,
                        cpu=args.cpu,
                        repetitions=args.repetitions,
                        events=events,
                        raw_base=raw_root / f"{slug}__{backend}__{group_name}",
                        force=args.force or attempt > 0,
                    )
                    cycles_stddev = result.get("cycles", {}).get("stddev_percent", 0.0)
                    if (
                        group_name != "core"
                        or cycles_stddev <= args.max_core_cycles_stddev
                        or attempt == args.stability_retries
                    ):
                        break
                    print(
                        f"  retry: cycles stddev={cycles_stddev:.2f}% exceeds "
                        f"{args.max_core_cycles_stddev:.2f}%",
                        flush=True,
                    )
                collected[group_name] = result
            case_results[backend] = collected

        for backend in ("cpp", "verilator"):
            rows.append(
                summarize(
                    case=case,
                    backend=backend,
                    logical_cycles=shared_cycles,
                    repetitions=args.repetitions,
                    baseline_cps=item[f"{backend}_cps_median"],
                    core=case_results[backend]["core"],
                    cache=case_results[backend]["cache"],
                )
            )
        write_results(
            output_root,
            {
                "date": dt.datetime.now().astimezone().isoformat(timespec="seconds"),
                "host": platform.node(),
                "kernel": platform.release(),
                "perf": checked_output(["perf", "--version"]),
                "cpu": args.cpu,
                "repetitions": args.repetitions,
                "target_seconds_for_faster_backend": args.target_seconds,
                "sample_every": args.sample_every,
                "event_groups": {"core": CORE_EVENTS, "cache": CACHE_EVENTS},
                "throughput_json": str(throughput_json),
            },
            rows,
        )

    if args.publish_csv:
        published = args.publish_csv.resolve()
        published.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(output_root / "results.csv", published)
        print(f"published CSV to {published}", flush=True)
    print(f"wrote {len(rows)} backend rows to {output_root}", flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
