#!/usr/bin/env python3
"""Verify read-only delay taps against unoptimized C++ and Verilog models."""

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
        raise RuntimeError(f"missing result for {command}")
    return int(match.group(1)), int(match.group(2))


def emit(pycc: Path, source: Path, output: Path, kind: str, mode: str) -> dict[str, object]:
    run([
        str(pycc), str(source), f"--emit={kind}",
        f"--state-delay-opt={mode}", "--state-pack-width=0", "-o", str(output),
    ])
    return json.loads(Path(f"{output}.stats.json").read_text(encoding="utf-8"))


def compile_cpp(cxx: str, header: Path, binary: Path) -> None:
    run([
        cxx, "-std=c++17", "-O2", "-DNDEBUG", f"-I{ROOT / 'runtime'}",
        f'-DMODEL_HEADER="{header}"', str(HERE / "state_delay_tap_model.cpp"),
        "-o", str(binary),
    ])


def compile_verilator(verilog: Path, obj_dir: Path) -> Path:
    env = dict(os.environ)
    env["CCACHE_DISABLE"] = "1"
    run([
        "verilator", "--cc", "--exe", "--build", "-j", "2", "-Wno-fatal",
        f"-I{ROOT / 'runtime/verilog'}", "--Mdir", str(obj_dir),
        "--top-module", "state_delay_tap_codegen", str(verilog),
        str(HERE / "state_delay_tap_verilator.cpp"),
    ], env=env)
    return obj_dir / "Vstate_delay_tap_codegen"


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--pycc", type=Path, default=DEFAULT_BUILD / "pycc")
    parser.add_argument("--cxx", default=os.environ.get("CXX", "c++"))
    args = parser.parse_args()

    source = HERE / "state_delay_tap_codegen.mlir"
    with tempfile.TemporaryDirectory(prefix="pyc-delay-tap-", dir="/tmp") as path:
        temp = Path(path)
        observed: dict[str, tuple[int, int]] = {}
        for variant, mode in (("off", "off"), ("tap", "structural")):
            cpp = temp / f"{variant}.hpp"
            verilog = temp / f"{variant}.v"
            cpp_stats = emit(args.pycc, source, cpp, "cpp", mode)
            verilog_stats = emit(args.pycc, source, verilog, "verilog", mode)
            for stats in (cpp_stats, verilog_stats):
                expected_taps = 1 if variant == "tap" else 0
                if stats.get("delay_chain_taps_created") != expected_taps:
                    raise AssertionError(f"{variant}: unexpected tap stats: {stats}")
                if stats.get("reg_count") != 4 or stats.get("reg_bits") != 32:
                    raise AssertionError(f"{variant}: logical state changed: {stats}")

            cpp_binary = temp / f"{variant}_cpp"
            compile_cpp(args.cxx, cpp, cpp_binary)
            observed[f"{variant}_cpp"] = result([str(cpp_binary)])
            verilator_binary = compile_verilator(verilog, temp / f"obj_{variant}")
            observed[f"{variant}_verilog"] = result([str(verilator_binary)])

        if len(set(observed.values())) != 1:
            raise AssertionError(f"delay tap backend mismatch: {observed}")
        cycles, checksum = next(iter(observed.values()))
        print(
            "delay tap equivalence verified: 4-reg chain -> one depth-4 history "
            f"with depth-2 tap; four models match for {cycles} cycles "
            f"(checksum={checksum})"
        )


if __name__ == "__main__":
    main()
