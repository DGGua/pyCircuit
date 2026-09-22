#!/usr/bin/env python3
"""Summarize module fallback timing JSONL emitted by generated C++."""

from __future__ import annotations

import argparse
import json
import statistics
import sys
from pathlib import Path

REQUIRED_FIELDS = (
    "path",
    "module",
    "eval_calls",
    "eval_total_ns",
    "topo_eval_calls",
    "topo_eval_ns",
    "fallback_calls",
    "fallback_total_ns",
    "initial_comb_pass_calls",
    "initial_comb_pass_ns",
    "fallback_comb_pass_calls",
    "fallback_comb_pass_ns",
    "fallback_primitive_ns",
    "fallback_iterations",
    "fallback_max_iterations",
    "fallback_iter_hist_0",
    "fallback_iter_hist_1",
    "fallback_iter_hist_2",
    "fallback_iter_hist_3",
    "fallback_iter_hist_4p",
    "instance_eval_calls",
    "instance_cache_skips",
    "primitive_eval_calls",
    "primitive_cache_skips",
)

COUNT_FIELDS = tuple(name for name in REQUIRED_FIELDS if name not in ("path", "module"))


class StatsError(Exception):
    pass


def _require_int(row: dict, key: str, where: str) -> int:
    value = row[key]
    if isinstance(value, bool) or not isinstance(value, int):
        raise StatsError(f"{where}: field {key} must be an integer, got {value!r}")
    if value < 0:
        raise StatsError(f"{where}: field {key} is negative")
    return value


def load_rows(path: Path) -> list[dict]:
    rows: list[dict] = []
    text = path.read_text(encoding="utf-8")
    if not text.strip():
        raise StatsError(f"{path}: empty stats file")
    for lineno, line in enumerate(text.splitlines(), 1):
        if not line.strip():
            continue
        where = f"{path}:{lineno}"
        try:
            row = json.loads(line)
        except json.JSONDecodeError as exc:
            raise StatsError(f"{where}: invalid JSONL: {exc}") from exc
        if not isinstance(row, dict):
            raise StatsError(f"{where}: expected a JSON object")
        missing = [name for name in REQUIRED_FIELDS if name not in row]
        if missing:
            raise StatsError(f"{where}: missing fields: {', '.join(missing)}")
        if not isinstance(row["path"], str) or not isinstance(row["module"], str):
            raise StatsError(f"{where}: path and module must be strings")
        for name in COUNT_FIELDS:
            _require_int(row, name, where)
        rows.append(row)
    if not rows:
        raise StatsError(f"{path}: no stats records")
    return rows


def _share(numerator: int, denominator: int, label: str) -> float | None:
    if denominator == 0:
        if numerator != 0:
            raise StatsError(f"divide by zero computing {label}: numerator={numerator}")
        return None
    return numerator / denominator


def _check_bounds(row: dict, where: str) -> None:
    eval_ns = row["eval_total_ns"]
    for name in (
        "topo_eval_ns",
        "fallback_total_ns",
        "initial_comb_pass_ns",
        "fallback_comb_pass_ns",
        "fallback_primitive_ns",
    ):
        if row[name] > eval_ns:
            raise StatsError(f"{where}: {name}={row[name]} exceeds eval_total_ns={eval_ns}")
    if row["initial_comb_pass_ns"] + row["fallback_total_ns"] > eval_ns:
        raise StatsError(
            f"{where}: initial_comb_pass_ns + fallback_total_ns exceeds eval_total_ns"
        )
    if row["fallback_primitive_ns"] + row["fallback_comb_pass_ns"] > row["fallback_total_ns"]:
        raise StatsError(
            f"{where}: fallback primitive+comb time exceeds fallback_total_ns"
        )


def summarize_file(path: Path, *, require_timing: bool) -> dict:
    rows = load_rows(path)
    for index, row in enumerate(rows, 1):
        _check_bounds(row, f"{path}:record{index}:{row['path']}")
    top = rows[0]
    if require_timing and top["eval_total_ns"] == 0:
        raise StatsError(f"{path}: top eval_total_ns is 0; timing was not recorded")
    instances = []
    for row in rows:
        eval_ns = row["eval_total_ns"]
        calls = row["fallback_calls"]
        instances.append(
            {
                "path": row["path"],
                "module": row["module"],
                "eval_calls": row["eval_calls"],
                "eval_total_ns": eval_ns,
                "fallback_calls": calls,
                "fallback_total_ns": row["fallback_total_ns"],
                "fallback_iterations": row["fallback_iterations"],
                "fallback_share": _share(row["fallback_total_ns"], eval_ns, f"{row['path']} fallback_share"),
                "avg_fallback_iterations": _share(
                    row["fallback_iterations"], calls, f"{row['path']} avg_fallback_iterations"
                ),
            }
        )
    ranked_share = sorted(
        (item for item in instances if item["fallback_share"] is not None),
        key=lambda item: (-item["fallback_share"], item["path"]),
    )
    ranked_iters = sorted(
        instances,
        key=lambda item: (-item["fallback_iterations"], item["path"]),
    )
    return {
        "file": str(path),
        "top_path": top["path"],
        "fallback_share": _share(top["fallback_total_ns"], top["eval_total_ns"], "top fallback_share"),
        "fallback_comb_share": _share(
            top["fallback_comb_pass_ns"], top["eval_total_ns"], "top fallback_comb_share"
        ),
        "initial_comb_share": _share(
            top["initial_comb_pass_ns"], top["eval_total_ns"], "top initial_comb_share"
        ),
        "topo_share": _share(top["topo_eval_ns"], top["eval_total_ns"], "top topo_share"),
        "top": {name: top[name] for name in REQUIRED_FIELDS},
        "instances": instances,
        "highest_fallback_share": ranked_share[:10],
        "highest_fallback_iterations": ranked_iters[:10],
        "note": "Child times are nested inside parent times and must not be summed.",
    }


def _median(values: list[float]) -> float:
    return float(statistics.median(values))


def aggregate(summaries: list[dict]) -> dict:
    shares = [item["fallback_share"] for item in summaries]
    if any(share is None for share in shares):
        raise StatsError("cannot aggregate runs whose top fallback_share is undefined")
    typed = [float(share) for share in shares]
    return {
        "runs": len(typed),
        "fallback_share_median": _median(typed),
        "fallback_share_min": min(typed),
        "fallback_share_max": max(typed),
        "note": "Child times are nested inside parent times and must not be summed.",
    }


def _fmt_share(value: float | None) -> str:
    if value is None:
        return "n/a"
    return f"{value * 100:.4f}%"


def render_markdown(summaries: list[dict], combined: dict | None) -> str:
    lines = ["# Module fallback timing", ""]
    lines.append("Child instance times are nested inside their parent and must not be added together.")
    lines.append("")
    if combined is not None:
        lines.append("## Repeated runs")
        lines.append("")
        lines.append(f"- runs: {combined['runs']}")
        lines.append(f"- fallback_share median: {_fmt_share(combined['fallback_share_median'])}")
        lines.append(f"- fallback_share min: {_fmt_share(combined['fallback_share_min'])}")
        lines.append(f"- fallback_share max: {_fmt_share(combined['fallback_share_max'])}")
        lines.append("")
    for summary in summaries:
        lines.append(f"## {summary['file']}")
        lines.append("")
        lines.append(f"- top: `{summary['top_path']}`")
        lines.append(f"- fallback_share: {_fmt_share(summary['fallback_share'])}")
        lines.append(f"- fallback_comb_share: {_fmt_share(summary['fallback_comb_share'])}")
        lines.append(f"- initial_comb_share: {_fmt_share(summary['initial_comb_share'])}")
        lines.append(f"- topo_share: {_fmt_share(summary['topo_share'])}")
        lines.append("")
        lines.append("| path | module | fallback_share | avg_iters | fallback_iterations |")
        lines.append("| --- | --- | --- | --- | --- |")
        for item in summary["highest_fallback_share"][:10]:
            avg = item["avg_fallback_iterations"]
            avg_text = "n/a" if avg is None else f"{avg:.3f}"
            lines.append(
                f"| `{item['path']}` | `{item['module']}` | {_fmt_share(item['fallback_share'])} | {avg_text} | {item['fallback_iterations']} |"
            )
        lines.append("")
    return "\n".join(lines)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("stats", nargs="+", type=Path, help="JSONL stats files")
    parser.add_argument("--out-json", type=Path)
    parser.add_argument("--out-md", type=Path)
    parser.add_argument(
        "--require-timing",
        action="store_true",
        help="Fail when the top record has eval_total_ns == 0",
    )
    args = parser.parse_args(argv)
    try:
        summaries = [summarize_file(path, require_timing=args.require_timing) for path in args.stats]
        combined = aggregate(summaries) if len(summaries) > 1 else None
        payload = {"runs": summaries, "aggregate": combined}
        rendered = render_markdown(summaries, combined)
    except StatsError as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1
    text = json.dumps(payload, indent=2)
    if args.out_json:
        args.out_json.write_text(text + "\n", encoding="utf-8")
    else:
        print(text)
    if args.out_md:
        args.out_md.write_text(rendered, encoding="utf-8")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
