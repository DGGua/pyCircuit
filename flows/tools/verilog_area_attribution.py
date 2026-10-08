#!/usr/bin/env python3
"""Verilog post-synthesis area attribution harness.

Runs the same input `.pyc` through several `pycc --emit=verilog` variants,
synthesizes each variant with the yosys script that pycc itself emits
(`yosys_synth.ys`), and reports a LUT/FF/carry/RAM comparison table.

Purpose: quantify WHERE the area gap vs. Chisel/hand-written RTL comes from
before changing any pass. See docs/gates/verilog-area-attribution.md.

Usage:
    # full run: pycc variants + synth + table
    python3 flows/tools/verilog_area_attribution.py \
        --pyc designs/XiangShan-pyc/build_out/bpu.pyc \
        --top bpu \
        --pycc-args "--hierarchy-policy=instantiate --inline-policy=off" \
        --out .pycircuit_out/area_attribution/bpu

    # synth+compare only, from already-emitted variant dirs
    python3 flows/tools/verilog_area_attribution.py --compare-only \
        --top bpu --out <dir-containing-variant-subdirs>

Exit code is 0 if synthesis succeeded for all variants, 1 otherwise.
"""
from __future__ import annotations

import argparse
import json
import os
import shutil
import subprocess
import sys
from pathlib import Path
from typing import Any

REPO_ROOT = Path(__file__).resolve().parents[2]

PYCC_SEARCH_PATHS = [
    REPO_ROOT / ".pycircuit_out" / "toolchain" / "install" / "bin" / "pycc",
    REPO_ROOT / "build" / "bin" / "pycc",
    REPO_ROOT / "compiler" / "mlir" / "build" / "bin" / "pycc",
]

# Each variant isolates one suspected cause of the area gap:
#   baseline          : as-is pipeline
#   flatten           : cross-instance optimization enabled (--flatten)
#   comb-struct-off   : disable comb fusion (--emit-structural=off)
#   flatten+nobox     : both combined
VARIANTS: dict[str, list[str]] = {
    "baseline": [],
    "flatten": ["--flatten"],
    "comb-struct-off": ["--emit-structural=off"],
    "flatten+comb-struct-off": ["--flatten", "--emit-structural=off"],
}

def find_pycc() -> Path:
    env = os.environ.get("PYCC")
    if env:
        p = Path(env)
        if p.is_file() and os.access(p, os.X_OK):
            return p
    for p in PYCC_SEARCH_PATHS:
        if p.is_file() and os.access(p, os.X_OK):
            return p
    raise SystemExit("pycc not found. Set PYCC=<path> or build the toolchain first.")


def run(cmd: list[str], **kw: Any) -> subprocess.CompletedProcess:
    return subprocess.run(cmd, capture_output=True, text=True, **kw)


def emit_variant(name: str, extra_args: list[str], pyc: Path, out_root: Path,
                 pycc: Path, base_args: list[str]) -> tuple[bool, str]:
    out_dir = out_root / name / "verilog"
    if out_dir.exists():
        shutil.rmtree(out_dir)
    out_dir.mkdir(parents=True)
    cmd = [str(pycc), str(pyc), "--emit=verilog", f"--out-dir={out_dir}",
           *base_args, *extra_args]
    r = run(cmd)
    if r.returncode != 0:
        return False, f"pycc failed for {name}:\n{r.stderr[-4000:]}"
    return True, ""


def synth_variant(name: str, out_root: Path, top: str) -> tuple[bool, str]:
    vdir = out_root / name / "verilog"
    ys = vdir / "yosys_synth.ys"
    if not ys.is_file():
        return False, f"{ys} missing (pycc did not emit it?)"
    stat_json = vdir / "yosys_stat.json"
    synth_log = vdir / "yosys_synth.log"
    # the generated .ys uses paths relative to the out-dir; flatten so that
    # flops inside pyc_reg/delay_line primitives are counted by `stat`
    r = run(["yosys", "-q", "-s", "yosys_synth.ys",
             "-p", "flatten; opt -fast",
             "-p", f"tee -o yosys_stat.json stat -json {top}"], cwd=vdir)
    synth_log.write_text(r.stdout + r.stderr)
    if r.returncode != 0:
        return False, f"yosys failed for {name} (see {synth_log})"
    return True, ""


def parse_stat(stat_json: Path) -> dict[str, int]:
    data = json.loads(stat_json.read_text())
    # stat -json emits: {"modules": {"\\name": {"num_cells_by_type": {...}}}}
    modules = data.get("modules", {})
    totals: dict[str, int] = {}
    for mod in modules.values():
        for cell, n in mod.get("num_cells_by_type", {}).items():
            totals[cell] = totals.get(cell, 0) + n

    def is_lut(k: str) -> bool:
        return ("lut" in k.lower() or "LUT" in k
                or k.startswith("$_") and k.endswith("_") and any(
                    g in k for g in ("AND", "OR", "XOR", "NOT", "MUX", "AOI", "OAI")))

    def is_ff(k: str) -> bool:
        return "DFF" in k

    def is_ram(k: str) -> bool:
        return "RAM" in k or "mem" in k.lower()

    def is_carry(k: str) -> bool:
        return "CARRY" in k or k in ("$fa", "$full_adder")

    out: dict[str, int] = {
        "__total_cells": sum(totals.values()),
        "__flops": sum(n for k, n in totals.items() if is_ff(k)),
        "__luts": sum(n for k, n in totals.items() if is_lut(k)),
        "__mux": sum(n for k, n in totals.items() if "MUX" in k),
        "__ram": sum(n for k, n in totals.items() if is_ram(k)),
        "__carry": sum(n for k, n in totals.items() if is_carry(k)),
    }
    # top-10 non-gate cell types (e.g. paramod pyc_reg wrappers) for insight
    others = sorted(((k, n) for k, n in totals.items()
                     if not (is_lut(k) or is_ff(k) or is_ram(k) or is_carry(k))),
                    key=lambda x: -x[1])[:10]
    for k, n in others:
        out[f"cell:{k}"] = n
    return out


def summarize(out_root: Path, top: str, variants: list[str]) -> tuple[bool, list[dict[str, Any]]]:
    rows, ok = [], True
    for name in variants:
        stat_json = out_root / name / "verilog" / "yosys_stat.json"
        if not stat_json.is_file():
            ok = False
            rows.append({"variant": name, "error": f"missing {stat_json}"})
            continue
        row = {"variant": name, **parse_stat(stat_json)}
        rows.append(row)
    base = next((r for r in rows if r.get("variant") == "baseline" and "__luts" in r), None)
    if base:
        for r in rows:
            if "__luts" in r:
                r["delta_luts_pct"] = round(
                    100.0 * (r["__luts"] - base["__luts"]) / max(1, base["__luts"]), 1)
                r["delta_flops_pct"] = round(
                    100.0 * (r["__flops"] - base["__flops"]) / max(1, base["__flops"]), 1)
    return ok, rows


def print_table(rows: list[dict[str, Any]]) -> None:
    hdr = f"{'variant':28s} {'LUTs':>9s} {'dLUT%':>7s} {'FFs':>9s} {'dFF%':>7s} {'RAM':>6s} {'CARRY':>6s} {'cells':>9s}"
    print(hdr)
    print("-" * len(hdr))
    for r in rows:
        if "error" in r:
            print(f"{r['variant']:28s}  ERROR: {r['error']}")
            continue
        print(f"{r['variant']:28s} {r['__luts']:9d} {r.get('delta_luts_pct', 0):7.1f} "
              f"{r['__flops']:9d} {r.get('delta_flops_pct', 0):7.1f} "
              f"{r['__ram']:6d} {r['__carry']:6d} {r['__total_cells']:9d}")


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--pyc", type=Path, help="input .pyc file")
    ap.add_argument("--top", required=True)
    ap.add_argument("--pycc-args", default="",
                    help="extra args passed to every pycc invocation (quoted string)")
    ap.add_argument("--variants", default=",".join(VARIANTS),
                    help="comma-separated subset of: " + ", ".join(VARIANTS))
    ap.add_argument("--out", type=Path, required=True)
    ap.add_argument("--compare-only", action="store_true",
                    help="skip pycc/yosys, just re-summarize existing variant dirs")
    args = ap.parse_args()

    names = [v.strip() for v in args.variants.split(",") if v.strip()]
    unknown = [v for v in names if v not in VARIANTS]
    if unknown:
        ap.error(f"unknown variants: {unknown}")

    args.out.mkdir(parents=True, exist_ok=True)
    errors: list[str] = []

    if not args.compare_only:
        if not args.pyc or not args.pyc.is_file():
            ap.error(f"--pyc not found: {args.pyc}")
        pycc = find_pycc()
        base_args = [a for a in args.pycc_args.split() if a]
        for name in names:
            ok, err = emit_variant(name, VARIANTS[name], args.pyc, args.out, pycc, base_args)
            if not ok:
                errors.append(err)
                continue
            ok, err = synth_variant(name, args.out, args.top)
            if not ok:
                errors.append(err)

    ok, rows = summarize(args.out, args.top, names)
    print_table(rows)
    summary = {"top": args.top, "rows": rows, "errors": errors}
    (args.out / "attribution_summary.json").write_text(json.dumps(summary, indent=2))
    # also emit markdown next to the summary for gates docs
    md = ["| variant | LUTs | dLUT% | FFs | dFF% | RAM | CARRY | cells |",
          "|---|---|---|---|---|---|---|---|"]
    for r in rows:
        if "error" in r:
            md.append(f"| {r['variant']} | ERROR | | | | | | |")
        else:
            md.append(f"| {r['variant']} | {r['__luts']} | {r.get('delta_luts_pct', 0)} "
                      f"| {r['__flops']} | {r.get('delta_flops_pct', 0)} "
                      f"| {r['__ram']} | {r['__carry']} | {r['__total_cells']} |")
    (args.out / "attribution_summary.md").write_text("\n".join(md) + "\n")
    return 0 if (ok and not errors) else 1


if __name__ == "__main__":
    sys.exit(main())
