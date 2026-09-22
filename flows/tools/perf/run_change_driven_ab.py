#!/usr/bin/env python3
"""Run reproducible wall-time A/B measurements for scheduler command templates."""

from __future__ import annotations

import argparse
import json
import math
import os
import statistics
import subprocess
import sys
import time
from pathlib import Path
from typing import Any

VARIANTS = ("current", "dag-always", "dag-dirty")


def _percentile(values: list[float], percentile: float) -> float:
    """Return the nearest-rank percentile, suitable for small gate samples."""
    ordered = sorted(values)
    rank = max(1, math.ceil(percentile * len(ordered)))
    return ordered[rank - 1]


def _expand(template: str, *, variant: str, run: int, output_dir: Path) -> str:
    try:
        return template.format(
            variant=variant,
            run=run,
            output_dir=str(output_dir),
        )
    except (KeyError, ValueError) as exc:
        raise ValueError(f"invalid {variant} command template: {exc}") from exc


def _run_once(
    template: str,
    *,
    variant: str,
    phase: str,
    run: int,
    output_dir: Path,
) -> float:
    run_dir = output_dir / variant / phase / str(run)
    run_dir.mkdir(parents=True, exist_ok=True)
    command = _expand(template, variant=variant, run=run, output_dir=run_dir)
    (run_dir / "command.txt").write_text(command + "\n", encoding="utf-8")
    env = os.environ.copy()
    env.update(
        {
            "PYC_AB_VARIANT": variant,
            "PYC_AB_PHASE": phase,
            "PYC_AB_RUN": str(run),
            "PYC_AB_OUTPUT_DIR": str(run_dir),
        }
    )
    start = time.perf_counter()
    with (
        (run_dir / "stdout.log").open("w", encoding="utf-8") as stdout,
        (run_dir / "stderr.log").open("w", encoding="utf-8") as stderr,
    ):
        result = subprocess.run(
            ["bash", "-lc", command],
            cwd=os.getcwd(),
            env=env,
            stdout=stdout,
            stderr=stderr,
            check=False,
        )
    elapsed = time.perf_counter() - start
    metadata = {
        "command": command,
        "elapsed_seconds": elapsed,
        "returncode": result.returncode,
    }
    (run_dir / "result.json").write_text(
        json.dumps(metadata, indent=2, sort_keys=True) + "\n", encoding="utf-8"
    )
    if result.returncode:
        raise RuntimeError(
            f"{variant} {phase} run {run} failed with exit {result.returncode}; "
            f"see {run_dir / 'stderr.log'}"
        )
    return elapsed


def run_benchmark(
    commands: dict[str, str],
    *,
    output_dir: Path,
    warmups: int = 1,
    repeats: int = 7,
) -> dict[str, Any]:
    """Warm each variant, then interleave measured runs in a fixed order."""
    if warmups < 0 or repeats < 1:
        raise ValueError("warmups must be >= 0 and repeats must be >= 1")
    output_dir.mkdir(parents=True, exist_ok=True)
    measured: dict[str, list[float]] = {variant: [] for variant in VARIANTS}
    for run in range(1, warmups + 1):
        for variant in VARIANTS:
            _run_once(
                commands[variant],
                variant=variant,
                phase="warmup",
                run=run,
                output_dir=output_dir,
            )
    for run in range(1, repeats + 1):
        for variant in VARIANTS:
            measured[variant].append(
                _run_once(
                    commands[variant],
                    variant=variant,
                    phase="measure",
                    run=run,
                    output_dir=output_dir,
                )
            )
    variants = {}
    for variant in VARIANTS:
        values = measured[variant]
        variants[variant] = {
            "command_template": commands[variant],
            "samples_seconds": values,
            "median_seconds": statistics.median(values),
            "p95_seconds": _percentile(values, 0.95),
            "min_seconds": min(values),
            "max_seconds": max(values),
        }
    current_median = variants["current"]["median_seconds"]
    for variant in VARIANTS:
        median = variants[variant]["median_seconds"]
        variants[variant]["speedup_vs_current"] = (
            current_median / median if median else None
        )
    return {
        "schema_version": 1,
        "warmups": warmups,
        "repeats": repeats,
        "measurement": "wall_clock_seconds",
        "order": list(VARIANTS),
        "variants": variants,
    }


def render_markdown(result: dict[str, Any]) -> str:
    lines = [
        "# Change-driven scheduler A/B",
        "",
        f"- warmups: {result['warmups']}",
        f"- measured repeats: {result['repeats']}",
        "- order per round: " + " → ".join(result["order"]),
        "",
        "| variant | median (s) | p95 (s) | min (s) | max (s) | speedup vs current |",
        "| --- | ---: | ---: | ---: | ---: | ---: |",
    ]
    for name in VARIANTS:
        item = result["variants"][name]
        speedup = item["speedup_vs_current"]
        speedup_text = "n/a" if speedup is None else f"{speedup:.4f}x"
        lines.append(
            f"| `{name}` | {item['median_seconds']:.6f} | "
            f"{item['p95_seconds']:.6f} | {item['min_seconds']:.6f} | "
            f"{item['max_seconds']:.6f} | {speedup_text} |"
        )
    lines.append("")
    return "\n".join(lines)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--current-cmd", required=True)
    parser.add_argument("--dag-always-cmd", required=True)
    parser.add_argument("--dag-dirty-cmd", required=True)
    parser.add_argument("--output-dir", required=True, type=Path)
    parser.add_argument("--warmups", type=int, default=1)
    parser.add_argument("--repeats", type=int, default=7)
    parser.add_argument(
        "--out-json",
        type=Path,
        help="Defaults to <output-dir>/summary.json",
    )
    parser.add_argument(
        "--out-md",
        type=Path,
        help="Defaults to <output-dir>/summary.md",
    )
    args = parser.parse_args(argv)
    commands = {
        "current": args.current_cmd,
        "dag-always": args.dag_always_cmd,
        "dag-dirty": args.dag_dirty_cmd,
    }
    try:
        result = run_benchmark(
            commands,
            output_dir=args.output_dir,
            warmups=args.warmups,
            repeats=args.repeats,
        )
        out_json = args.out_json or args.output_dir / "summary.json"
        out_md = args.out_md or args.output_dir / "summary.md"
        out_json.parent.mkdir(parents=True, exist_ok=True)
        out_md.parent.mkdir(parents=True, exist_ok=True)
        out_json.write_text(
            json.dumps(result, indent=2, sort_keys=True) + "\n", encoding="utf-8"
        )
        out_md.write_text(render_markdown(result), encoding="utf-8")
        sys.stdout.write(json.dumps(result, indent=2, sort_keys=True) + "\n")
    except (OSError, RuntimeError, ValueError) as exc:
        sys.stderr.write(f"error: {exc}\n")
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
