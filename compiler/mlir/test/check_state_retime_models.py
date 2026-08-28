#!/usr/bin/env python3
"""Cross-check baseline, delay-only, and retimed pipeline models."""

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


def emit(pycc: Path, output: Path, kind: str, options: list[str]) -> dict[str, object]:
    run([
        str(pycc), str(HERE / "state_retime_codegen.mlir"), f"--emit={kind}",
        "--state-pack-width=0", *options, "-o", str(output),
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

    variants = {
        "off": ["--state-delay-opt=off", "--state-retime=off"],
        "delay_only": ["--state-delay-opt=structural", "--state-retime=off"],
        "retimed": ["--state-delay-opt=structural", "--state-retime=pipeline"],
        "default": ["--state-delay-opt=structural"],
    }
    with tempfile.TemporaryDirectory(prefix="pyc-retime-", dir="/tmp") as path:
        temp = Path(path)
        observed: dict[str, tuple[int, int]] = {}
        for variant, options in variants.items():
            cpp = temp / f"{variant}.hpp"
            verilog = temp / f"{variant}.v"
            cpp_stats = emit(args.pycc, cpp, "cpp", options)
            verilog_stats = emit(args.pycc, verilog, "verilog", options)
            for stats in (cpp_stats, verilog_stats):
                retime_enabled = variant in ("retimed", "default")
                expected_regs = 5 if retime_enabled else 6
                expected_bits = 33 if retime_enabled else 48
                if (stats.get("reg_count") != expected_regs or
                        stats.get("reg_bits") != expected_bits):
                    raise AssertionError(f"{variant}: logical state changed: {stats}")
                expected = 2 if retime_enabled else 0
                if stats.get("retime_regions_rewritten") != expected:
                    raise AssertionError(f"{variant}: retiming stats mismatch: {stats}")
                expected_bits_removed = 15 if retime_enabled else 0
                if stats.get("retime_state_bits_removed") != expected_bits_removed:
                    raise AssertionError(f"{variant}: retiming bit delta mismatch: {stats}")
                expected_lines = 0 if variant == "off" else 1
                if stats.get("delay_line_count") != expected_lines:
                    raise AssertionError(f"{variant}: delay line mismatch: {stats}")

            cpp_binary = temp / f"{variant}_cpp"
            compile_cpp(args.cxx, cpp, cpp_binary)
            observed[f"{variant}_cpp"] = result([str(cpp_binary)])
            verilator_binary = compile_verilator(verilog, temp / f"obj_{variant}")
            observed[f"{variant}_verilog"] = result([str(verilator_binary)])

        if len(set(observed.values())) != 1:
            raise AssertionError(f"retiming model mismatch: {observed}")
        cycles, checksum = next(iter(observed.values()))
        print(
            "retiming verified: a 3-register computed pipeline -> one depth-3 "
            "history and two delayed i8 operands -> one i1 result state; "
            "off/delay-only/retimed/default C++ and Verilog match for "
            f"{cycles} cycles (checksum={checksum})"
        )


if __name__ == "__main__":
    main()
