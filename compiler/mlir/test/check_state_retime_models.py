#!/usr/bin/env python3
"""Cross-check retimed C++ against unoptimized Verilog."""

from __future__ import annotations

import argparse
import json
import os
import re
import subprocess
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[2]
DEFAULT_BUILD = ROOT / ".pycircuit_out/toolchain/build-delay-line/bin"
RESULT_RE = re.compile(r"cycles=(\d+) checksum=(\d+)")


def run(command: list[str], *, env: dict[str, str] | None = None) -> str:
    return subprocess.run(
        command, cwd=ROOT, check=True, text=True, capture_output=True, env=env
    ).stdout


def result(command: list[str]) -> tuple[int, int]:
    match = RESULT_RE.search(run(command))
    if not match:
        raise RuntimeError(f"missing model result: {command}")
    return int(match.group(1)), int(match.group(2))


def emit(pycc: Path, output: Path, kind: str) -> dict[str, object]:
    run([
        str(pycc), str(HERE / "state_retime_codegen.mlir"), f"--emit={kind}",
        "--state-pack-width=0", "-o", str(output),
    ])
    return json.loads(Path(f"{output}.stats.json").read_text(encoding="utf-8"))


def compile_cpp(cxx: str, header: Path, binary: Path) -> None:
    run([
        cxx, "-std=c++17", "-O2", f"-I{ROOT / 'runtime'}",
        f'-DMODEL_HEADER="{header}"', str(HERE / "state_retime_model.cpp"),
        "-o", str(binary),
    ])


def compile_verilator(verilog: Path, obj_dir: Path) -> Path:
    env = dict(os.environ)
    env["CCACHE_DISABLE"] = "1"
    run([
        "verilator", "--cc", "--exe", "--build", "-j", "2", "-Wno-fatal",
        f"-I{ROOT / 'runtime/verilog'}", "--Mdir", str(obj_dir),
        "--top-module", "state_retime_codegen", str(verilog),
        str(HERE / "state_retime_verilator.cpp"),
    ], env=env)
    return obj_dir / "Vstate_retime_codegen"


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--pycc", type=Path, default=DEFAULT_BUILD / "pycc")
    parser.add_argument("--cxx", default=os.environ.get("CXX", "c++"))
    args = parser.parse_args()

    with tempfile.TemporaryDirectory(prefix="pyc-retime-", dir="/tmp") as path:
        temp = Path(path)
        cpp = temp / "default.hpp"
        verilog = temp / "default.v"
        observed: dict[str, tuple[int, int]] = {}
        cpp_stats = emit(args.pycc, cpp, "cpp")
        verilog_stats = emit(args.pycc, verilog, "verilog")
        if (cpp_stats.get("reg_count") != 5 or cpp_stats.get("reg_bits") != 33):
            raise AssertionError(f"cpp: logical state changed: {cpp_stats}")
        if cpp_stats.get("retime_regions_rewritten") != 2:
            raise AssertionError(f"cpp: retiming stats mismatch: {cpp_stats}")
        if cpp_stats.get("retime_state_bits_removed") != 15:
            raise AssertionError(f"cpp: retiming bit delta mismatch: {cpp_stats}")
        if cpp_stats.get("delay_line_count") != 1:
            raise AssertionError(f"cpp: delay line mismatch: {cpp_stats}")
        if cpp_stats.get("state_opt_policy") != "structural":
            raise AssertionError(f"cpp: policy is not structural: {cpp_stats}")
        if cpp_stats.get("state_retime_policy") != "pipeline":
            raise AssertionError(f"cpp: retime policy is not pipeline: {cpp_stats}")
        if (verilog_stats.get("reg_count") != 6 or
                verilog_stats.get("reg_bits") != 48):
            raise AssertionError(
                f"verilog: unoptimized state changed: {verilog_stats}"
            )
        if verilog_stats.get("retime_regions_rewritten"):
            raise AssertionError(
                f"verilog: retiming ran unexpectedly: {verilog_stats}"
            )
        if verilog_stats.get("delay_line_count"):
            raise AssertionError(
                f"verilog: delay line formed unexpectedly: {verilog_stats}"
            )
        if verilog_stats.get("state_opt_policy") != "off":
            raise AssertionError(f"verilog: policy is not off: {verilog_stats}")
        if verilog_stats.get("state_retime_policy") != "off":
            raise AssertionError(
                f"verilog: retime policy is not off: {verilog_stats}"
            )

        compile_cpp(args.cxx, cpp, temp / "cpp_bin")
        observed["cpp"] = result([str(temp / "cpp_bin")])
        observed["verilog"] = result([str(compile_verilator(verilog, temp / "obj"))])
        if len(set(observed.values())) != 1:
            raise AssertionError(f"retiming model mismatch: {observed}")
        cycles, checksum = next(iter(observed.values()))
        print(
            "retiming verified: a 3-register computed pipeline -> one depth-3 "
            "history and two delayed i8 operands -> one i1 result state; "
            "optimized C++ matches unoptimized Verilog for "
            f"{cycles} cycles (checksum={checksum})"
        )


if __name__ == "__main__":
    main()
