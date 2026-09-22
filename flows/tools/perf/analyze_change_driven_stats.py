#!/usr/bin/env python3
"""Validate and summarize hierarchical change-driven scheduler JSONL stats."""

from __future__ import annotations

import argparse
import json
import statistics
import sys
from pathlib import Path
from typing import Any

IDENTITY_FIELDS = ("path", "module")
COUNT_FIELDS = (
    "source_checks",
    "source_changes",
    "dirty_enqueues",
    "coalesced",
    "stage_eval_calls",
    "comb_eval_calls",
    "output_publish_attempts",
    "semantic_changes",
    "max_ready",
    "commit_changes",
)
REQUIRED_FIELDS = IDENTITY_FIELDS + COUNT_FIELDS
RATIO_FIELDS = (
    "source_change_rate",
    "coalesced_rate",
    "comb_calls_per_stage_eval",
    "semantic_change_rate",
)


class StatsError(Exception):
    """Raised when scheduler statistics violate the expected schema."""


def _ratio(numerator: int, denominator: int, label: str) -> float | None:
    if denominator == 0:
        if numerator:
            raise StatsError(
                f"{label}: numerator is {numerator}, but denominator is zero"
            )
        return None
    return numerator / denominator


def _validate_row(row: Any, where: str) -> dict[str, Any]:
    if not isinstance(row, dict):
        raise StatsError(f"{where}: expected a JSON object")
    missing = [field for field in REQUIRED_FIELDS if field not in row]
    unknown = sorted(set(row) - set(REQUIRED_FIELDS))
    if missing:
        raise StatsError(f"{where}: missing fields: {', '.join(missing)}")
    if unknown:
        raise StatsError(f"{where}: unknown fields: {', '.join(unknown)}")
    for field in IDENTITY_FIELDS:
        if not isinstance(row[field], str) or not row[field]:
            raise StatsError(f"{where}: field {field} must be a non-empty string")
    for field in COUNT_FIELDS:
        value = row[field]
        if isinstance(value, bool) or not isinstance(value, int) or value < 0:
            raise StatsError(f"{where}: field {field} must be a non-negative integer")
    if row["source_changes"] > row["source_checks"]:
        raise StatsError(f"{where}: source_changes exceeds source_checks")
    if row["semantic_changes"] > row["output_publish_attempts"]:
        raise StatsError(f"{where}: semantic_changes exceeds output_publish_attempts")
    return row


def load_rows(path: Path) -> list[dict[str, Any]]:
    """Load one non-empty JSON object per line and enforce the exact schema."""
    try:
        lines = path.read_text(encoding="utf-8").splitlines()
    except OSError as exc:
        raise StatsError(f"{path}: cannot read stats file: {exc}") from exc
    rows: list[dict[str, Any]] = []
    paths: set[str] = set()
    for lineno, line in enumerate(lines, 1):
        if not line.strip():
            continue
        where = f"{path}:{lineno}"
        try:
            raw = json.loads(line)
        except json.JSONDecodeError as exc:
            raise StatsError(f"{where}: invalid JSON: {exc}") from exc
        row = _validate_row(raw, where)
        if row["path"] in paths:
            raise StatsError(f"{where}: duplicate path {row['path']!r}")
        paths.add(row["path"])
        rows.append(row)
    if not rows:
        raise StatsError(f"{path}: no JSONL records")
    return rows


def _derived(row: dict[str, Any], where: str) -> dict[str, float | None]:
    enqueue_attempts = row["dirty_enqueues"] + row["coalesced"]
    return {
        "source_change_rate": _ratio(
            row["source_changes"], row["source_checks"], f"{where} source change rate"
        ),
        "coalesced_rate": _ratio(
            row["coalesced"], enqueue_attempts, f"{where} coalesced rate"
        ),
        "comb_calls_per_stage_eval": _ratio(
            row["comb_eval_calls"],
            row["stage_eval_calls"],
            f"{where} comb calls per stage eval",
        ),
        "semantic_change_rate": _ratio(
            row["semantic_changes"],
            row["output_publish_attempts"],
            f"{where} semantic change rate",
        ),
    }


def summarize_file(path: Path) -> dict[str, Any]:
    """Return a top-record summary plus validated hierarchical records."""
    rows = load_rows(path)
    records: list[dict[str, Any]] = []
    for row in rows:
        records.append({**row, **_derived(row, f"{path}:{row['path']}")})
    top = records[0]
    return {
        "file": str(path),
        "top_path": top["path"],
        "top": top,
        "records": records,
        "note": (
            "Records are hierarchical; parent and child counters may overlap and "
            "must not be summed."
        ),
    }


def _distribution(values: list[int | float]) -> dict[str, int | float]:
    return {
        "median": statistics.median(values),
        "min": min(values),
        "max": max(values),
    }


def aggregate(summaries: list[dict[str, Any]]) -> dict[str, Any]:
    """Aggregate top-level metrics across repeated runs."""
    if not summaries:
        raise StatsError("cannot aggregate zero runs")
    top_paths = {summary["top_path"] for summary in summaries}
    if len(top_paths) != 1:
        raise StatsError(
            "repeated runs have different top paths: " + ", ".join(sorted(top_paths))
        )
    metrics: dict[str, dict[str, int | float] | None] = {}
    for field in COUNT_FIELDS + RATIO_FIELDS:
        values = [summary["top"][field] for summary in summaries]
        if any(value is None for value in values):
            if not all(value is None for value in values):
                raise StatsError(
                    f"metric {field} is undefined in only some repeated runs"
                )
            metrics[field] = None
        else:
            metrics[field] = _distribution(values)
    return {
        "runs": len(summaries),
        "top_path": summaries[0]["top_path"],
        "metrics": metrics,
    }


def _format_value(field: str, value: int | float | None) -> str:
    if value is None:
        return "n/a"
    if field.endswith("_rate"):
        return f"{float(value) * 100:.4f}%"
    if isinstance(value, float):
        return f"{value:.4f}"
    return str(value)


def render_markdown(
    summaries: list[dict[str, Any]], combined: dict[str, Any] | None
) -> str:
    """Render summaries without adding overlapping hierarchy counters."""
    lines = [
        "# Change-driven scheduler statistics",
        "",
        "Parent and child counters may overlap; hierarchical records are not summed.",
        "",
    ]
    if combined is not None:
        lines.extend(
            [
                "## Repeated runs",
                "",
                f"- runs: {combined['runs']}",
                f"- top: `{combined['top_path']}`",
                "",
                "| metric | median | min | max |",
                "| --- | ---: | ---: | ---: |",
            ]
        )
        for field, dist in combined["metrics"].items():
            if dist is None:
                values = ("n/a", "n/a", "n/a")
            else:
                values = tuple(
                    _format_value(field, dist[key]) for key in ("median", "min", "max")
                )
            lines.append(f"| `{field}` | {values[0]} | {values[1]} | {values[2]} |")
        lines.append("")
    for summary in summaries:
        lines.extend(
            [
                f"## {summary['file']}",
                "",
                f"- top: `{summary['top_path']}`",
                "",
                "| path | source checks/changes | enqueue/coalesced | stage/comb evals | "
                "publish/semantic changes | max ready | commit changes |",
                "| --- | ---: | ---: | ---: | ---: | ---: | ---: |",
            ]
        )
        for row in summary["records"]:
            lines.append(
                f"| `{row['path']}` | {row['source_checks']}/{row['source_changes']} "
                f"| {row['dirty_enqueues']}/{row['coalesced']} "
                f"| {row['stage_eval_calls']}/{row['comb_eval_calls']} "
                f"| {row['output_publish_attempts']}/{row['semantic_changes']} "
                f"| {row['max_ready']} | {row['commit_changes']} |"
            )
        lines.append("")
    return "\n".join(lines)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("stats", nargs="+", type=Path, help="scheduler JSONL files")
    parser.add_argument("--out-json", type=Path)
    parser.add_argument("--out-md", type=Path)
    args = parser.parse_args(argv)
    try:
        summaries = [summarize_file(path) for path in args.stats]
        combined = aggregate(summaries) if len(summaries) > 1 else None
        payload = {"runs": summaries, "aggregate": combined}
        markdown = render_markdown(summaries, combined)
        encoded = json.dumps(payload, indent=2, sort_keys=True) + "\n"
        if args.out_json:
            args.out_json.parent.mkdir(parents=True, exist_ok=True)
            args.out_json.write_text(encoded, encoding="utf-8")
        else:
            sys.stdout.write(encoded)
        if args.out_md:
            args.out_md.parent.mkdir(parents=True, exist_ok=True)
            args.out_md.write_text(markdown, encoding="utf-8")
    except (OSError, StatsError) as exc:
        sys.stderr.write(f"error: {exc}\n")
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
