#!/usr/bin/env python3
"""Compare generated C++ vs Rust functional-sim performance (v1 subset).

Uses the same .pyc, the same step protocol, and -O3 / opt-level=3. This is a
naive full-eval comparison; production C++ still has extra caches/SCC/PGO.
"""

from __future__ import annotations

import argparse
import json
import statistics
import sys
import time
from datetime import datetime, timezone
from pathlib import Path

REPO = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(REPO / "tests" / "rust_emit"))
from runner import (  # noqa: E402
    REPO_ROOT,
    build_runtime_rlib,
    compile_cpp_harness,
    compile_rust_harness,
    emit_pyc,
    find_pycc,
    find_rustc,
    pycc_emit,
    run_cmd,
)

HARNESS = REPO / "tests" / "rust_emit" / "harness"
DESIGNS = {
    "counter": {
        "src": REPO / "designs" / "examples" / "counter" / "counter.py",
        "rust": HARNESS / "counter_main.rs",
        "cpp": HARNESS / "counter_main.cpp",
        "cycles_flag": "cycles_counter",
        "default_cycles": 2000000,
    },
    "arith": {
        "src": REPO / "designs" / "examples" / "arith" / "arith.py",
        "rust": HARNESS / "arith_main.rs",
        "cpp": HARNESS / "arith_main.cpp",
        "cycles_flag": "cycles_arith",
        "default_cycles": 1000000,
    },
    "microbench": {
        "src": REPO / "tests" / "rust_emit" / "designs" / "microbench.py",
        "rust": HARNESS / "microbench_main.rs",
        "cpp": HARNESS / "microbench_main.cpp",
        "cycles_flag": "cycles_microbench",
        "default_cycles": 50000,
    },
}


def _file_size(path: Path) -> int:
    return path.stat().st_size if path.is_file() else 0


def _median_json_hz(exe: Path, cycles: int, repeats: int) -> dict:
    hz_vals = []
    last = {}
    for _ in range(repeats):
        proc = run_cmd([str(exe), "perf", str(cycles)])
        line = [ln for ln in proc.stdout.splitlines() if ln.startswith("{")][-1]
        last = json.loads(line)
        hz_vals.append(float(last["hz"]))
    last["hz"] = statistics.median(hz_vals)
    last["hz_samples"] = hz_vals
    return last


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument(
        "--out",
        default=str(REPO / ".pycircuit_out" / "perf" / "rust_vs_cpp.json"),
    )
    ap.add_argument("--repeats", type=int, default=3)
    ap.add_argument("--cycles-counter", type=int, default=2000000)
    ap.add_argument("--cycles-arith", type=int, default=5000000)
    ap.add_argument("--cycles-microbench", type=int, default=200000)
    args = ap.parse_args()

    pycc = find_pycc()
    if find_rustc() is None:
        raise SystemExit("rustc is required for this comparison")

    work = REPO / ".pycircuit_out" / "perf" / "rust_vs_cpp_work"
    work.mkdir(parents=True, exist_ok=True)
    rlib = build_runtime_rlib(work)
    rows = []
    cycles_by_name = {
        "counter": args.cycles_counter,
        "arith": args.cycles_arith,
        "microbench": args.cycles_microbench,
    }

    for name, spec in DESIGNS.items():
        case_dir = work / name
        case_dir.mkdir(parents=True, exist_ok=True)
        pyc = emit_pyc(spec["src"], case_dir / f"{name}.pyc", pycc)
        t0 = time.perf_counter()
        rust_src = pycc_emit(pyc, kind="rust", out=case_dir / f"{name}.rs", pycc=pycc)
        rust_emit_s = time.perf_counter() - t0
        t0 = time.perf_counter()
        cpp_src = pycc_emit(pyc, kind="cpp", out=case_dir / f"{name}.cpp", pycc=pycc)
        cpp_emit_s = time.perf_counter() - t0

        t0 = time.perf_counter()
        rust_exe = compile_rust_harness(spec["rust"], rust_src, rlib, case_dir / f"{name}_rs")
        rust_compile_s = time.perf_counter() - t0
        t0 = time.perf_counter()
        cpp_exe = compile_cpp_harness(spec["cpp"], cpp_src, case_dir / f"{name}_cpp")
        cpp_compile_s = time.perf_counter() - t0

        run_cmd([str(rust_exe)])
        run_cmd([str(cpp_exe)])

        cycles = int(cycles_by_name[name])
        rust_perf = _median_json_hz(rust_exe, cycles, args.repeats)
        cpp_perf = _median_json_hz(cpp_exe, cycles, args.repeats)
        rows.append(
            {
                "design": name,
                "cycles": cycles,
                "repeats": args.repeats,
                "cpp": {
                    "emit_s": cpp_emit_s,
                    "compile_s": cpp_compile_s,
                    "binary_bytes": _file_size(cpp_exe),
                    **cpp_perf,
                },
                "rust": {
                    "emit_s": rust_emit_s,
                    "compile_s": rust_compile_s,
                    "binary_bytes": _file_size(rust_exe),
                    **rust_perf,
                },
            }
        )

    out = {
        "generated_at": datetime.now(timezone.utc).isoformat(),
        "pycc": str(pycc),
        "opt": {"cxx": "-O3", "rustc": "-C opt-level=3"},
        "note": "Naive full-eval comparison. Production C++ may use caches/SCC/PGO/-Os.",
        "designs": rows,
    }
    out_path = Path(args.out)
    out_path.parent.mkdir(parents=True, exist_ok=True)
    out_path.write_text(json.dumps(out, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(out, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
