#!/usr/bin/env python3
"""Build and benchmark pycc C++ models against Verilated RTL.

The script consumes a run tree that already contains, for every case:

    <case>/cpp/cpp_compile_manifest.json
    <case>/verilog/<top>.v
    <case>/verilog/pyc_primitives.v

It finds the matching PYC IR under --source-root, generates paired C++
harnesses with the same stimulus/checksum schedule, builds both backends with
GCC -O2 -DNDEBUG, checks sampled output equivalence, calibrates the cycle
count, and records repeated throughput measurements.
"""

from __future__ import annotations

import argparse
import csv
import json
import os
import re
import shutil
import statistics
import subprocess
import sys
import time
from dataclasses import dataclass
from pathlib import Path


@dataclass(frozen=True)
class Port:
    name: str
    width: int
    kind: str


@dataclass(frozen=True)
class Case:
    key: str
    case_dir: Path
    pyc: Path
    target: str
    top_header: str
    inputs: tuple[Port, ...]
    outputs: tuple[Port, ...]


def run_checked(cmd: list[str], *, cwd: Path, env: dict[str, str], log: Path) -> float:
    log.parent.mkdir(parents=True, exist_ok=True)
    start = time.monotonic()
    with log.open("w", encoding="utf-8") as out:
        out.write("command: " + " ".join(cmd) + "\n")
        out.flush()
        proc = subprocess.run(cmd, cwd=cwd, env=env, stdout=out, stderr=subprocess.STDOUT, text=True)
    elapsed = time.monotonic() - start
    if proc.returncode != 0:
        raise RuntimeError(f"command failed ({proc.returncode}); see {log}")
    return elapsed


def width_of(ty: str) -> tuple[int, str]:
    ty = ty.strip()
    if ty == "!pyc.clock":
        return 1, "clock"
    if ty == "!pyc.reset":
        return 1, "reset"
    m = re.fullmatch(r"i([0-9]+)", ty)
    if not m:
        raise ValueError(f"unsupported top-level port type: {ty}")
    return int(m.group(1)), "data"


def split_types(text: str) -> list[str]:
    text = text.strip()
    if text.startswith("(") and text.endswith(")"):
        text = text[1:-1]
    if not text:
        return []
    return [item.strip() for item in text.split(",")]


def parse_pyc(pyc: Path) -> tuple[str, tuple[Port, ...], tuple[Port, ...]]:
    text = pyc.read_text(encoding="utf-8")
    top_match = re.search(r"pyc\.top\s*=\s*@([A-Za-z_][A-Za-z0-9_]*)", text)
    if not top_match:
        raise ValueError(f"missing pyc.top in {pyc}")
    top = top_match.group(1)
    line = next((ln for ln in text.splitlines() if ln.startswith(f"func.func @{top}(")), "")
    if not line:
        raise ValueError(f"missing top func.func @{top} in {pyc}")
    sig = re.match(rf"func\.func @{re.escape(top)}\((.*?)\) -> (.*?) attributes \{{", line)
    if not sig:
        raise ValueError(f"cannot parse top signature in {pyc}")

    arg_names_match = re.search(r"arg_names\s*=\s*(\[[^]]*\])", line)
    result_names_match = re.search(r"result_names\s*=\s*(\[[^]]*\])", line)
    if not arg_names_match or not result_names_match:
        raise ValueError(f"missing port name attributes in {pyc}")
    arg_names = json.loads(arg_names_match.group(1))
    result_names = json.loads(result_names_match.group(1))

    arg_types: list[str] = []
    for entry in split_types(sig.group(1)):
        m = re.fullmatch(r"%[A-Za-z_][A-Za-z0-9_]*:\s*(.+)", entry)
        if not m:
            raise ValueError(f"cannot parse argument {entry!r} in {pyc}")
        arg_types.append(m.group(1).strip())
    result_types = split_types(sig.group(2))
    if len(arg_names) != len(arg_types) or len(result_names) != len(result_types):
        raise ValueError(f"port name/type count mismatch in {pyc}")

    inputs = []
    for name, ty in zip(arg_names, arg_types):
        width, kind = width_of(ty)
        inputs.append(Port(str(name), width, kind))
    outputs = []
    for name, ty in zip(result_names, result_types):
        width, kind = width_of(ty)
        outputs.append(Port(str(name), width, kind))
    return top, tuple(inputs), tuple(outputs)


def discover_cases(run_root: Path, source_root: Path, selected: set[str]) -> list[Case]:
    cases: list[Case] = []
    for manifest_path in sorted(run_root.rglob("cpp/cpp_compile_manifest.json")):
        case_dir = manifest_path.parent.parent
        key = case_dir.relative_to(run_root).as_posix()
        manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
        target = str(manifest["target_name"])
        top_header = str(manifest["top_header"])
        if selected and key not in selected and target not in selected:
            continue
        source_dir = source_root / key
        pycs = sorted(source_dir.glob("*.pyc"))
        if len(pycs) != 1:
            raise RuntimeError(f"expected one PYC input for {key}, found {len(pycs)}")
        pyc = pycs[0]
        top, inputs, outputs = parse_pyc(pyc)
        if top != target:
            raise RuntimeError(f"top mismatch for {key}: IR={top}, C++ manifest={target}")
        cases.append(Case(key, case_dir, pyc, target, top_header, inputs, outputs))
    if selected:
        found = {c.key for c in cases} | {c.target for c in cases}
        missing = selected - found
        if missing:
            raise RuntimeError(f"unknown selected cases: {sorted(missing)}")
    return cases


def stim_expr(port_index: int, word_index: int) -> str:
    salt = ((port_index + 1) * 0xD1B54A32D192ED03 + word_index * 0x94D049BB133111EB) & ((1 << 64) - 1)
    return f"(stim ^ 0x{salt:016x}ULL)"


def cpp_assign(port: Port, port_index: int) -> list[str]:
    words = (port.width + 63) // 64
    vals = ", ".join(stim_expr(port_index, wi) for wi in range(words))
    if words == 1:
        return [f"    dut.{port.name} = pyc::cpp::Wire<{port.width}>({vals});"]
    return [f"    dut.{port.name} = pyc::cpp::Wire<{port.width}>({{{vals}}});"]


def verilator_assign(port: Port, port_index: int) -> list[str]:
    if port.width <= 64:
        mask = (1 << port.width) - 1 if port.width < 64 else (1 << 64) - 1
        return [f"    dut.{port.name} = ({stim_expr(port_index, 0)} & 0x{mask:016x}ULL);"]
    lines: list[str] = []
    words64 = (port.width + 63) // 64
    for wi in range(words64):
        var = f"stim_{port_index}_{wi}"
        lines.append(f"    const std::uint64_t {var} = {stim_expr(port_index, wi)};")
        lo32 = wi * 2
        if lo32 * 32 < port.width:
            remaining = port.width - lo32 * 32
            mask = (1 << min(32, remaining)) - 1
            lines.append(
                f"    dut.{port.name}[{lo32}] = static_cast<std::uint32_t>({var}) & 0x{mask:08x}U;"
            )
        if (lo32 + 1) * 32 < port.width:
            remaining = port.width - (lo32 + 1) * 32
            mask = (1 << min(32, remaining)) - 1
            lines.append(
                f"    dut.{port.name}[{lo32 + 1}] = static_cast<std::uint32_t>({var} >> 32) & 0x{mask:08x}U;"
            )
    return lines


def cpp_output_words(port: Port) -> list[str]:
    return [f"dut.{port.name}.word({wi})" for wi in range((port.width + 63) // 64)]


def verilator_output_words(port: Port) -> list[str]:
    if port.width <= 64:
        mask = (1 << port.width) - 1 if port.width < 64 else (1 << 64) - 1
        return [f"(static_cast<std::uint64_t>(dut.{port.name}) & 0x{mask:016x}ULL)"]
    words = []
    for wi in range((port.width + 63) // 64):
        lo32 = wi * 2
        expr = f"static_cast<std::uint64_t>(dut.{port.name}[{lo32}])"
        if (lo32 + 1) * 32 < port.width:
            expr = f"({expr} | (static_cast<std::uint64_t>(dut.{port.name}[{lo32 + 1}]) << 32))"
        remaining = port.width - wi * 64
        if remaining < 64:
            mask = (1 << remaining) - 1
            expr = f"(({expr}) & 0x{mask:016x}ULL)"
        words.append(expr)
    return words


def checksum_lines(case: Case, backend: str) -> list[str]:
    lines = ["      std::uint64_t observed = checksum;"]
    tag = 1
    for port in case.outputs:
        words = cpp_output_words(port) if backend == "cpp" else verilator_output_words(port)
        for expr in words:
            lines.append(f"      observed = mix64(observed ^ ({expr}) ^ 0x{tag:016x}ULL ^ i);")
            tag += 1
    lines.append("      checksum = observed;")
    return lines


def common_prelude() -> str:
    return """#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iomanip>
#include <iostream>

static inline std::uint64_t mix64(std::uint64_t x) {
  x += 0x9e3779b97f4a7c15ULL;
  x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ULL;
  x = (x ^ (x >> 27)) * 0x94d049bb133111ebULL;
  return x ^ (x >> 31);
}
"""


def generate_cpp_harness(case: Case) -> str:
    clocks = [p for p in case.inputs if p.kind == "clock"]
    resets = [p for p in case.inputs if p.kind == "reset"]
    data = [p for p in case.inputs if p.kind == "data"]
    init = []
    for p in clocks:
        init.append(f"  dut.{p.name} = pyc::cpp::Wire<1>(0);")
    for p in resets:
        init.append(f"  dut.{p.name} = pyc::cpp::Wire<1>(1);")
    for p in data:
        init.append(f"  dut.{p.name} = pyc::cpp::Wire<{p.width}>(0);")
    high = " ".join(f"dut.{p.name} = pyc::cpp::Wire<1>(1);" for p in clocks)
    low = " ".join(f"dut.{p.name} = pyc::cpp::Wire<1>(0);" for p in clocks)
    deassert = " ".join(f"dut.{p.name} = pyc::cpp::Wire<1>(0);" for p in resets)
    assigns: list[str] = []
    for idx, p in enumerate(data):
        assigns.extend(cpp_assign(p, idx))
    checks = checksum_lines(case, "cpp")
    return common_prelude() + f"""#include \"{case.top_header}\"

int main(int argc, char **argv) {{
  const std::uint64_t cycles = argc > 1 ? std::strtoull(argv[1], nullptr, 10) : 1000000ULL;
  const std::uint64_t sample_every = argc > 2 ? std::strtoull(argv[2], nullptr, 10) : 4096ULL;
  if (sample_every == 0 || (sample_every & (sample_every - 1)) != 0) return 2;
  pyc::gen::{case.target} dut;
{os.linesep.join(init)}
  auto one_cycle = [&]() {{
    dut.comb();
    {high}
    dut.tick(); dut.transfer(); dut.comb();
    {low}
    dut.tick(); dut.transfer();
  }};
  one_cycle(); one_cycle();
  {deassert}
  one_cycle();
  std::uint64_t checksum = 0;
  const auto begin = std::chrono::steady_clock::now();
  for (std::uint64_t i = 0; i < cycles; ++i) {{
    const std::uint64_t stim = i * 0x9e3779b97f4a7c15ULL + 0x243f6a8885a308d3ULL;
{os.linesep.join(assigns)}
    one_cycle();
    if ((i & (sample_every - 1)) == 0) {{
{os.linesep.join(checks)}
    }}
  }}
  const auto end = std::chrono::steady_clock::now();
  const double seconds = std::chrono::duration<double>(end - begin).count();
  std::cout << std::setprecision(12)
            << \"backend=cpp case={case.key} cycles=\" << cycles
            << \" seconds=\" << seconds
            << \" cycles_per_second=\" << (cycles / seconds)
            << \" checksum=\" << checksum
            << \" sample_every=\" << sample_every << \"\\n\";
  return 0;
}}
"""


def generate_verilator_harness(case: Case) -> str:
    clocks = [p for p in case.inputs if p.kind == "clock"]
    resets = [p for p in case.inputs if p.kind == "reset"]
    data = [p for p in case.inputs if p.kind == "data"]
    init = [f"  dut.{p.name} = 0;" for p in clocks]
    init.extend(f"  dut.{p.name} = 1;" for p in resets)
    for p in data:
        if p.width <= 64:
            init.append(f"  dut.{p.name} = 0;")
        else:
            for word in range((p.width + 31) // 32):
                init.append(f"  dut.{p.name}[{word}] = 0;")
    high = " ".join(f"dut.{p.name} = 1;" for p in clocks)
    low = " ".join(f"dut.{p.name} = 0;" for p in clocks)
    deassert = " ".join(f"dut.{p.name} = 0;" for p in resets)
    assigns: list[str] = []
    for idx, p in enumerate(data):
        assigns.extend(verilator_assign(p, idx))
    checks = checksum_lines(case, "verilator")
    return common_prelude() + f"""#include \"V{case.target}.h\"
#include \"verilated.h\"

int main(int argc, char **argv) {{
  Verilated::commandArgs(argc, argv);
  const std::uint64_t cycles = argc > 1 ? std::strtoull(argv[1], nullptr, 10) : 1000000ULL;
  const std::uint64_t sample_every = argc > 2 ? std::strtoull(argv[2], nullptr, 10) : 4096ULL;
  if (sample_every == 0 || (sample_every & (sample_every - 1)) != 0) return 2;
  V{case.target} dut;
{os.linesep.join(init)}
  auto one_cycle = [&]() {{
    dut.eval();
    {high}
    dut.eval();
    {low}
    dut.eval();
  }};
  one_cycle(); one_cycle();
  {deassert}
  one_cycle();
  std::uint64_t checksum = 0;
  const auto begin = std::chrono::steady_clock::now();
  for (std::uint64_t i = 0; i < cycles; ++i) {{
    const std::uint64_t stim = i * 0x9e3779b97f4a7c15ULL + 0x243f6a8885a308d3ULL;
{os.linesep.join(assigns)}
    one_cycle();
    if ((i & (sample_every - 1)) == 0) {{
{os.linesep.join(checks)}
    }}
  }}
  const auto end = std::chrono::steady_clock::now();
  const double seconds = std::chrono::duration<double>(end - begin).count();
  std::cout << std::setprecision(12)
            << \"backend=verilator case={case.key} cycles=\" << cycles
            << \" seconds=\" << seconds
            << \" cycles_per_second=\" << (cycles / seconds)
            << \" checksum=\" << checksum
            << \" sample_every=\" << sample_every << \"\\n\";
  dut.final();
  return 0;
}}
"""


def generate(case: Case) -> Path:
    bench = case.case_dir / "bench"
    bench.mkdir(parents=True, exist_ok=True)
    (bench / "bench_cpp.cpp").write_text(generate_cpp_harness(case), encoding="utf-8")
    (bench / "bench_verilator.cpp").write_text(generate_verilator_harness(case), encoding="utf-8")
    metadata = {
        "key": case.key,
        "target": case.target,
        "pyc": str(case.pyc),
        "inputs": [p.__dict__ for p in case.inputs],
        "outputs": [p.__dict__ for p in case.outputs],
    }
    (bench / "case.json").write_text(json.dumps(metadata, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    return bench


def build_case(case: Case, repo: Path, env: dict[str, str], jobs: int) -> dict[str, float]:
    bench = generate(case)
    cpp_dir = case.case_dir / "cpp"
    runtime_include = repo / ".pycircuit_out/toolchain/install-local-llvm19/include"
    sources = sorted(cpp_dir.glob("*.cpp"))
    cpp_cmd = [
        "g++", "-std=c++17", "-O2", "-DNDEBUG",
        "-I", str(cpp_dir), "-I", str(runtime_include),
        str(bench / "bench_cpp.cpp"), *(str(p) for p in sources),
        "-o", str(bench / "bench_cpp"),
    ]
    cpp_seconds = run_checked(cpp_cmd, cwd=repo, env=env, log=bench / "build_cpp.log")

    verilog_dir = case.case_dir / "verilog"
    verilogs = sorted(verilog_dir.glob("*.v"), key=lambda p: (p.name != "pyc_primitives.v", p.name))
    if not verilogs:
        raise RuntimeError(f"no Verilog sources for {case.key}")
    obj_dir = bench / "verilator_obj"
    verilator_cmd = [
        "verilator", "--cc", "--exe", "--build", "-j", str(jobs),
        "--compiler", "gcc", "--threads", "1", "-Wno-fatal",
        "--top-module", case.target, "--prefix", f"V{case.target}",
        "--Mdir", str(obj_dir), "-CFLAGS", "-DNDEBUG",
        "-MAKEFLAGS", "OPT_FAST=-O2 OPT_SLOW=-O2 OPT_GLOBAL=-O2",
        "-o", "bench_verilator", *(str(p) for p in verilogs), str(bench / "bench_verilator.cpp"),
    ]
    verilator_seconds = run_checked(
        verilator_cmd, cwd=repo, env=env, log=bench / "build_verilator.log"
    )
    return {"cpp_build_seconds": cpp_seconds, "verilator_build_seconds": verilator_seconds}


def parse_result(text: str) -> dict[str, str]:
    line = next((ln for ln in reversed(text.splitlines()) if ln.startswith("backend=")), "")
    if not line:
        raise ValueError(f"missing benchmark result in output: {text[-500:]}")
    result: dict[str, str] = {}
    for token in line.split():
        key, value = token.split("=", 1)
        result[key] = value
    return result


def run_binary(binary: Path, cycles: int, sample_every: int, cpu: int | None) -> dict[str, str]:
    cmd = [str(binary), str(cycles), str(sample_every)]
    if cpu is not None:
        cmd = ["taskset", "-c", str(cpu), *cmd]
    proc = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, check=False)
    if proc.returncode != 0:
        raise RuntimeError(f"benchmark failed ({proc.returncode}): {' '.join(cmd)}\n{proc.stdout}")
    return parse_result(proc.stdout)


def round_cycles(value: float, minimum: int = 100_000, maximum: int = 20_000_000) -> int:
    bounded = max(minimum, min(maximum, int(value)))
    quantum = 10_000
    return max(minimum, (bounded // quantum) * quantum)


def benchmark_case(
    case: Case,
    *,
    repetitions: int,
    target_seconds: float,
    cpu: int | None,
) -> dict[str, object]:
    bench = case.case_dir / "bench"
    cpp_bin = bench / "bench_cpp"
    verilator_bin = bench / "verilator_obj" / "bench_verilator"
    if not cpp_bin.is_file() or not verilator_bin.is_file():
        raise RuntimeError(f"missing benchmark binaries for {case.key}")

    verify_cycles = 4096
    cpp_verify = run_binary(cpp_bin, verify_cycles, 1, cpu)
    verilator_verify = run_binary(verilator_bin, verify_cycles, 1, cpu)
    equivalent = cpp_verify["checksum"] == verilator_verify["checksum"]
    if not equivalent:
        raise RuntimeError(
            f"checksum mismatch for {case.key}: cpp={cpp_verify['checksum']} verilator={verilator_verify['checksum']}"
        )

    calibration_cycles = 100_000
    cpp_cal = run_binary(cpp_bin, calibration_cycles, 4096, cpu)
    ver_cal = run_binary(verilator_bin, calibration_cycles, 4096, cpu)
    cpp_cps = float(cpp_cal["cycles_per_second"])
    ver_cps = float(ver_cal["cycles_per_second"])
    cycles = round_cycles(min(cpp_cps, ver_cps) * target_seconds)

    cpp_runs: list[dict[str, str]] = []
    ver_runs: list[dict[str, str]] = []
    for rep in range(repetitions):
        if rep % 2 == 0:
            cpp_runs.append(run_binary(cpp_bin, cycles, 4096, cpu))
            ver_runs.append(run_binary(verilator_bin, cycles, 4096, cpu))
        else:
            ver_runs.append(run_binary(verilator_bin, cycles, 4096, cpu))
            cpp_runs.append(run_binary(cpp_bin, cycles, 4096, cpu))

    cpp_values = [float(r["cycles_per_second"]) for r in cpp_runs]
    ver_values = [float(r["cycles_per_second"]) for r in ver_runs]
    cpp_median = statistics.median(cpp_values)
    ver_median = statistics.median(ver_values)
    return {
        "key": case.key,
        "target": case.target,
        "status": "pass",
        "equivalent_4096_cycles": equivalent,
        "verify_checksum": cpp_verify["checksum"],
        "cycles": cycles,
        "sample_every": 4096,
        "repetitions": repetitions,
        "cpp_cps_runs": cpp_values,
        "verilator_cps_runs": ver_values,
        "cpp_cps_median": cpp_median,
        "verilator_cps_median": ver_median,
        "verilator_over_cpp": ver_median / cpp_median,
        "cpp_executable_bytes": cpp_bin.stat().st_size,
        "verilator_executable_bytes": verilator_bin.stat().st_size,
    }


def default_cpu() -> int | None:
    try:
        affinity = sorted(os.sched_getaffinity(0))
    except (AttributeError, OSError):
        return None
    return affinity[0] if affinity else None


def write_results(out_dir: Path, rows: list[dict[str, object]], environment: dict[str, object]) -> None:
    out_dir.mkdir(parents=True, exist_ok=True)
    payload = {"environment": environment, "results": rows}
    (out_dir / "results.json").write_text(json.dumps(payload, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    fields = [
        "key", "target", "status", "equivalent_4096_cycles", "verify_checksum", "cycles",
        "sample_every", "repetitions", "cpp_cps_median", "verilator_cps_median",
        "verilator_over_cpp", "cpp_executable_bytes", "verilator_executable_bytes", "error",
    ]
    with (out_dir / "results.csv").open("w", encoding="utf-8", newline="") as f:
        writer = csv.DictWriter(f, fieldnames=fields, extrasaction="ignore")
        writer.writeheader()
        writer.writerows(rows)


def version_line(cmd: list[str]) -> str:
    proc = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, check=False)
    return proc.stdout.splitlines()[0] if proc.stdout else "unknown"


def main() -> int:
    repo_default = Path(__file__).resolve().parents[3]
    ap = argparse.ArgumentParser()
    ap.add_argument("--repo", type=Path, default=repo_default)
    ap.add_argument("--run-root", type=Path, required=True)
    ap.add_argument("--source-root", type=Path, required=True)
    ap.add_argument("--cases", default="", help="comma-separated case keys or top names")
    ap.add_argument("--jobs", type=int, default=4)
    ap.add_argument("--repetitions", type=int, default=3)
    ap.add_argument("--target-seconds", type=float, default=0.5)
    ap.add_argument("--cpu", type=int, default=None)
    ap.add_argument("--skip-build", action="store_true")
    args = ap.parse_args()

    repo = args.repo.resolve()
    run_root = args.run_root.resolve()
    source_root = args.source_root.resolve()
    selected = {x.strip() for x in args.cases.split(",") if x.strip()}
    cpu = args.cpu if args.cpu is not None else default_cpu()
    cases = discover_cases(run_root, source_root, selected)
    if not cases:
        raise SystemExit("no cases discovered")

    env = os.environ.copy()
    env["CCACHE_DIR"] = str((run_root / "ccache").resolve())
    env["CCACHE_TEMPDIR"] = str((run_root / "ccache-tmp").resolve())
    Path(env["CCACHE_DIR"]).mkdir(parents=True, exist_ok=True)
    Path(env["CCACHE_TEMPDIR"]).mkdir(parents=True, exist_ok=True)

    environment: dict[str, object] = {
        "date": time.strftime("%Y-%m-%dT%H:%M:%S%z"),
        "repo": str(repo),
        "run_root": str(run_root),
        "source_root": str(source_root),
        "gxx": version_line(["g++", "--version"]),
        "verilator": version_line(["verilator", "--version"]),
        "optimization": "-O2 -DNDEBUG",
        "trace": "off",
        "threads": 1,
        "cpu_affinity": cpu,
        "repetitions": args.repetitions,
        "target_seconds": args.target_seconds,
    }
    rows: list[dict[str, object]] = []
    results_dir = run_root / "results"

    for index, case in enumerate(cases, start=1):
        print(f"[{index}/{len(cases)}] {case.key}", flush=True)
        row: dict[str, object] = {"key": case.key, "target": case.target, "status": "failed"}
        try:
            if not args.skip_build:
                row.update(build_case(case, repo, env, max(1, args.jobs)))
            row.update(
                benchmark_case(
                    case,
                    repetitions=max(1, args.repetitions),
                    target_seconds=max(0.05, args.target_seconds),
                    cpu=cpu,
                )
            )
            print(
                f"  cpp={float(row['cpp_cps_median']):.3f} cyc/s "
                f"verilator={float(row['verilator_cps_median']):.3f} cyc/s "
                f"ratio={float(row['verilator_over_cpp']):.3f}",
                flush=True,
            )
        except Exception as exc:
            row["error"] = str(exc)
            print(f"  FAILED: {exc}", file=sys.stderr, flush=True)
        rows.append(row)
        write_results(results_dir, rows, environment)

    failed = sum(1 for row in rows if row.get("status") != "pass")
    print(f"completed={len(rows) - failed} failed={failed} results={results_dir}")
    return 1 if failed else 0


if __name__ == "__main__":
    raise SystemExit(main())
