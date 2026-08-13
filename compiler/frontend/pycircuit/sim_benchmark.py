from __future__ import annotations

import argparse
import hashlib
import json
import math
import os
import platform
import re
import shlex
import shutil
import statistics
import subprocess
import sys
import time
from dataclasses import dataclass
from datetime import datetime, timezone
from pathlib import Path
from typing import Any, Iterable, Mapping, Sequence

from .api_contract import nearest_project_root
from .packaged_toolchain import tool_executable


RESULT_PREFIX = "PYC_BENCH_RESULT "
OBS_PREFIX = "PYC_BENCH_OBS "
RESULT_SCHEMA_VERSION = 1
MAX_INPUT_TABLE_BYTES = 256 * 1024 * 1024
_INT_TYPE_RE = re.compile(r"^i([1-9][0-9]*)$")

# The emitters currently preserve C/C++ keywords. Rejecting them here turns an
# otherwise obscure native compiler failure into a useful interface diagnostic.
_CPP_KEYWORDS = {
    "alignas",
    "alignof",
    "and",
    "and_eq",
    "asm",
    "atomic_cancel",
    "atomic_commit",
    "atomic_noexcept",
    "auto",
    "bitand",
    "bitor",
    "bool",
    "break",
    "case",
    "catch",
    "char",
    "char16_t",
    "char32_t",
    "class",
    "compl",
    "concept",
    "const",
    "consteval",
    "constexpr",
    "constinit",
    "const_cast",
    "continue",
    "co_await",
    "co_return",
    "co_yield",
    "decltype",
    "default",
    "delete",
    "do",
    "double",
    "dynamic_cast",
    "else",
    "enum",
    "explicit",
    "export",
    "extern",
    "false",
    "float",
    "for",
    "friend",
    "goto",
    "if",
    "inline",
    "int",
    "long",
    "mutable",
    "namespace",
    "new",
    "noexcept",
    "not",
    "not_eq",
    "nullptr",
    "operator",
    "or",
    "or_eq",
    "private",
    "protected",
    "public",
    "reflexpr",
    "register",
    "reinterpret_cast",
    "requires",
    "return",
    "short",
    "signed",
    "sizeof",
    "static",
    "static_assert",
    "static_cast",
    "struct",
    "switch",
    "synchronized",
    "template",
    "this",
    "thread_local",
    "throw",
    "true",
    "try",
    "typedef",
    "typeid",
    "typename",
    "union",
    "unsigned",
    "using",
    "virtual",
    "void",
    "volatile",
    "wchar_t",
    "while",
    "xor",
    "xor_eq",
}


class BenchmarkError(RuntimeError):
    """A user-facing benchmark preparation, verification, or run error."""


@dataclass(frozen=True)
class Port:
    raw_name: str
    emitted_name: str
    type_text: str
    direction: str
    width: int
    role: str

    @property
    def words32(self) -> int:
        return (self.width + 31) // 32

    @property
    def words64(self) -> int:
        return (self.width + 63) // 64


@dataclass(frozen=True)
class Interface:
    top: str
    emitted_top: str
    inputs: tuple[Port, ...]
    outputs: tuple[Port, ...]
    mode: str

    @property
    def clocks(self) -> tuple[Port, ...]:
        return tuple(p for p in self.inputs if p.role == "clock")

    @property
    def resets(self) -> tuple[Port, ...]:
        return tuple(p for p in self.inputs if p.role == "reset")

    @property
    def data_inputs(self) -> tuple[Port, ...]:
        return tuple(p for p in self.inputs if p.role == "data")

    @property
    def observation(self) -> str:
        return "xfer" if self.mode == "clocked" else "settled-comb"

    @property
    def operation(self) -> str:
        return "cycles" if self.mode == "clocked" else "evaluations"


@dataclass(frozen=True)
class CommandResult:
    wall_seconds: float
    stdout: str
    stderr: str


@dataclass(frozen=True)
class NativeResult:
    backend: str
    mode: str
    iterations: int
    seconds: float
    throughput_per_second: float
    verify_digest: str
    digest: str
    final_digest: str
    external_wall_seconds: float


def _sanitize_id(value: str) -> str:
    out = "".join(c if (c.isascii() and (c.isalnum() or c == "_")) else "_" for c in str(value))
    if not out or out[0].isdigit():
        out = "_" + out
    return out


def _unique_emitted_names(raw_names: Sequence[str]) -> list[str]:
    used: dict[str, int] = {}
    result: list[str] = []
    for raw in raw_names:
        base = _sanitize_id(raw)
        used[base] = used.get(base, 0) + 1
        n = used[base]
        result.append(base if n == 1 else f"{base}_{n}")
    return result


def _parse_port_type(type_text: str, *, port_name: str, direction: str, max_port_bits: int) -> tuple[int, str]:
    if direction == "input" and type_text == "!pyc.clock":
        return (1, "clock")
    if direction == "input" and type_text == "!pyc.reset":
        return (1, "reset")
    match = _INT_TYPE_RE.fullmatch(type_text)
    if match is None:
        raise BenchmarkError(
            f"benchmark interface error: port {port_name!r} has unsupported type {type_text!r}; "
            "supported top-level types are flat iN values plus !pyc.clock/!pyc.reset inputs"
        )
    width = int(match.group(1))
    if width > max_port_bits:
        raise BenchmarkError(
            f"benchmark interface error: port {port_name!r} is {width} bits, exceeding "
            f"--max-port-bits={max_port_bits}"
        )
    return (width, "data")


def load_interface(
    manifest: Mapping[str, Any],
    *,
    requested_mode: str = "auto",
    max_port_bits: int = 4096,
) -> Interface:
    top = str(manifest.get("top", "")).strip()
    if not top:
        raise BenchmarkError("project manifest is missing `top`")
    rows = manifest.get("modules")
    if not isinstance(rows, list):
        raise BenchmarkError("project manifest is missing the `modules` list")
    top_row = next((row for row in rows if isinstance(row, Mapping) and str(row.get("name", "")) == top), None)
    if top_row is None:
        raise BenchmarkError(f"project manifest has no module row for top {top!r}")

    in_names = top_row.get("arg_names")
    in_types = top_row.get("arg_types")
    out_names = top_row.get("result_names")
    out_types = top_row.get("result_types")
    if not all(isinstance(value, list) for value in (in_names, in_types, out_names, out_types)):
        raise BenchmarkError("top module manifest has malformed port arrays")
    assert isinstance(in_names, list)
    assert isinstance(in_types, list)
    assert isinstance(out_names, list)
    assert isinstance(out_types, list)
    if len(in_names) != len(in_types) or len(out_names) != len(out_types):
        raise BenchmarkError("top module manifest port name/type arity mismatch")

    raw_names = [str(x) for x in [*in_names, *out_names]]
    emitted_names = _unique_emitted_names(raw_names)
    inputs: list[Port] = []
    outputs: list[Port] = []
    for index, (raw, type_text) in enumerate(zip(in_names, in_types)):
        name = str(raw)
        ty = str(type_text)
        width, role = _parse_port_type(ty, port_name=name, direction="input", max_port_bits=max_port_bits)
        emitted = emitted_names[index]
        if emitted in _CPP_KEYWORDS:
            raise BenchmarkError(f"benchmark interface error: emitted input name {emitted!r} is a C++ keyword")
        inputs.append(Port(name, emitted, ty, "input", width, role))
    offset = len(inputs)
    for index, (raw, type_text) in enumerate(zip(out_names, out_types)):
        name = str(raw)
        ty = str(type_text)
        width, role = _parse_port_type(ty, port_name=name, direction="output", max_port_bits=max_port_bits)
        emitted = emitted_names[offset + index]
        if emitted in _CPP_KEYWORDS:
            raise BenchmarkError(f"benchmark interface error: emitted output name {emitted!r} is a C++ keyword")
        outputs.append(Port(name, emitted, ty, "output", width, role))
    if not outputs:
        raise BenchmarkError("benchmark interface error: top module has no outputs to verify")

    clocks = [p for p in inputs if p.role == "clock"]
    resets = [p for p in inputs if p.role == "reset"]
    if len(clocks) > 1:
        names = ", ".join(p.raw_name for p in clocks)
        raise BenchmarkError(f"benchmark interface error: found multiple clocks ({names}); v1 supports at most one clock")
    if len(resets) > 1:
        names = ", ".join(p.raw_name for p in resets)
        raise BenchmarkError(f"benchmark interface error: found multiple resets ({names}); v1 supports at most one reset")

    mode = str(requested_mode).strip().lower()
    if mode not in {"auto", "comb", "clocked"}:
        raise BenchmarkError(f"invalid benchmark mode: {requested_mode!r}")
    if mode == "auto":
        mode = "clocked" if clocks else "comb"
    if mode == "clocked" and not clocks:
        raise BenchmarkError("--mode=clocked requires one !pyc.clock input")

    emitted_top = _sanitize_id(top)
    if emitted_top in _CPP_KEYWORDS:
        raise BenchmarkError(f"benchmark interface error: emitted top name {emitted_top!r} is a C++ keyword")
    return Interface(top, emitted_top, tuple(inputs), tuple(outputs), mode)


def _cpp_string(value: str) -> str:
    return json.dumps(str(value), ensure_ascii=True)


def _top_mask(width: int, word_bits: int) -> int:
    remainder = width % word_bits
    return (1 << remainder) - 1 if remainder else (1 << word_bits) - 1


def _frame_u64_expr(port: Port, word64_index: int) -> str:
    lo = 2 * word64_index
    hi = lo + 1
    expr = f"static_cast<std::uint64_t>(frame.{port.emitted_name}[{lo}])"
    if hi < port.words32:
        expr = f"({expr} | (static_cast<std::uint64_t>(frame.{port.emitted_name}[{hi}]) << 32u))"
    if word64_index + 1 == port.words64 and port.width % 64:
        expr = f"({expr} & 0x{_top_mask(port.width, 64):x}ull)"
    return expr


def _verilator_output_u64_expr(port: Port, word64_index: int) -> str:
    if port.width <= 64:
        expr = f"static_cast<std::uint64_t>(dut.{port.emitted_name})"
    else:
        lo = 2 * word64_index
        hi = lo + 1
        expr = f"static_cast<std::uint64_t>(dut.{port.emitted_name}.at({lo}))"
        if hi < port.words32:
            expr = f"({expr} | (static_cast<std::uint64_t>(dut.{port.emitted_name}.at({hi})) << 32u))"
    if word64_index + 1 == port.words64 and port.width % 64:
        expr = f"({expr} & 0x{_top_mask(port.width, 64):x}ull)"
    return expr


def render_common_harness_header(interface: Interface, *, input_table_size: int) -> str:
    frame_fields = "".join(
        f"  std::array<std::uint32_t, {port.words32}> {port.emitted_name}{{}};\n" for port in interface.data_inputs
    )
    fill_lines: list[str] = []
    for port in interface.data_inputs:
        fill_lines.append(f"    for (auto &word : frame.{port.emitted_name}) word = static_cast<std::uint32_t>(splitmix64(state));\n")
        if port.width % 32:
            fill_lines.append(
                f"    frame.{port.emitted_name}[{port.words32 - 1}] &= 0x{_top_mask(port.width, 32):x}u;\n"
            )
    if not fill_lines:
        fill_lines.append("    (void)frame;\n")
    return f"""// Generated by pyCircuit simulator benchmark. Do not edit.
#pragma once
#include <array>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <limits>
#include <memory>
#include <string>

namespace pyc_bench {{

constexpr std::size_t kInputTableSize = {input_table_size}u;
constexpr std::size_t kInputTableMask = kInputTableSize - 1u;
static_assert((kInputTableSize & kInputTableMask) == 0u,
              "benchmark input table size must be a power of two");

struct InputFrame {{
{frame_fields}}};

using InputTable = std::array<InputFrame, kInputTableSize>;

inline std::uint64_t splitmix64(std::uint64_t &state) {{
  state += 0x9e3779b97f4a7c15ull;
  std::uint64_t value = state;
  value = (value ^ (value >> 30u)) * 0xbf58476d1ce4e5b9ull;
  value = (value ^ (value >> 27u)) * 0x94d049bb133111ebull;
  return value ^ (value >> 31u);
}}

inline std::unique_ptr<InputTable> makeInputs(std::uint64_t seed) {{
  auto inputs = std::make_unique<InputTable>();
  std::uint64_t state = seed;
  for (auto &frame : *inputs) {{
{"".join(fill_lines)}  }}
  return inputs;
}}

inline std::uint64_t parseU64(int argc, char **argv, int index, std::uint64_t fallback) {{
  if (argc <= index) return fallback;
  char *end = nullptr;
  errno = 0;
  const auto value = std::strtoull(argv[index], &end, 0);
  return (end != argv[index] && *end == '\\0' && errno != ERANGE) ? value : fallback;
}}

inline void mix(std::uint64_t &hash, std::uint64_t value) {{
  hash ^= value + 0x9e3779b97f4a7c15ull + (hash << 6u) + (hash >> 2u);
}}

inline void emitObservation(std::uint64_t cycle, const char *port, unsigned width,
                            unsigned word, std::uint64_t value) {{
  std::cout << "{OBS_PREFIX}cycle=" << cycle << " port=" << port
            << " width=" << width << " word=" << word << " value=0x"
            << std::hex << std::setw(16) << std::setfill('0') << value
            << std::dec << std::setfill(' ') << "\\n";
}}

inline void emitResult(const char *backend, const char *mode,
                       std::uint64_t iterations, double seconds,
                       std::uint64_t verifyDigest, std::uint64_t digest,
                       std::uint64_t finalDigest) {{
  const double throughput = seconds > 0.0 ? static_cast<double>(iterations) / seconds : 0.0;
  std::cout << "{RESULT_PREFIX}{{\\\"schema_version\\\":{RESULT_SCHEMA_VERSION},"
            << "\\\"backend\\\":\\\"" << backend << "\\\","
            << "\\\"mode\\\":\\\"" << mode << "\\\","
            << "\\\"iterations\\\":" << iterations << ","
            << std::setprecision(17)
            << "\\\"seconds\\\":" << seconds << ","
            << "\\\"throughput_per_second\\\":" << throughput << ","
            << "\\\"verify_digest\\\":\\\"0x" << std::hex << verifyDigest << "\\\","
            << "\\\"digest\\\":\\\"0x" << digest << "\\\","
            << "\\\"final_digest\\\":\\\"0x" << finalDigest << "\\\"}}"
            << std::dec << "\\n";
}}

}} // namespace pyc_bench
"""


def _render_cpp_apply_inputs(interface: Interface) -> str:
    lines = ["inline void applyInputs(Dut &dut, const InputFrame &frame) {\n"]
    for port in interface.data_inputs:
        words = ", ".join(_frame_u64_expr(port, index) for index in range(port.words64))
        lines.append(f"  dut.{port.emitted_name} = pyc::cpp::Wire<{port.width}>({{{words}}});\n")
    if not interface.data_inputs:
        lines.append("  (void)dut; (void)frame;\n")
    lines.append("}\n")
    return "".join(lines)


def _render_verilator_apply_inputs(interface: Interface) -> str:
    lines = ["inline void applyInputs(Dut &dut, const InputFrame &frame) {\n"]
    for port in interface.data_inputs:
        if port.width <= 64:
            lines.append(f"  dut.{port.emitted_name} = {_frame_u64_expr(port, 0)};\n")
        else:
            for index in range(port.words32):
                value = f"frame.{port.emitted_name}[{index}]"
                if index + 1 == port.words32 and port.width % 32:
                    value = f"({value} & 0x{_top_mask(port.width, 32):x}u)"
                lines.append(f"  dut.{port.emitted_name}.at({index}) = {value};\n")
    if not interface.data_inputs:
        lines.append("  (void)dut; (void)frame;\n")
    lines.append("}\n")
    return "".join(lines)


def _render_observe(interface: Interface, *, backend: str) -> str:
    lines = [
        "inline void observe(const Dut &dut, std::uint64_t cycle, bool transcript, std::uint64_t &hash) {\n",
        "  mix(hash, cycle);\n",
    ]
    for port_index, port in enumerate(interface.outputs):
        lines.append(f"  mix(hash, {port_index}ull);\n")
        lines.append(f"  mix(hash, {port.width}ull);\n")
        for word_index in range(port.words64):
            if backend == "cpp":
                expr = f"dut.{port.emitted_name}.word({word_index})"
                if word_index + 1 == port.words64 and port.width % 64:
                    expr = f"({expr} & 0x{_top_mask(port.width, 64):x}ull)"
            else:
                expr = _verilator_output_u64_expr(port, word_index)
            variable = f"value_{port_index}_{word_index}"
            lines.append(f"  const std::uint64_t {variable} = {expr};\n")
            lines.append(f"  mix(hash, {word_index}ull);\n")
            lines.append(f"  mix(hash, {variable});\n")
            lines.append(
                f"  if (transcript) emitObservation(cycle, {_cpp_string(port.raw_name)}, {port.width}u, "
                f"{word_index}u, {variable});\n"
            )
    lines.append("}\n")
    return "".join(lines)


def _render_init(interface: Interface, *, backend: str) -> str:
    lines = ["inline void initialize(Dut &dut) {\n"]
    for port in interface.clocks:
        if backend == "cpp":
            lines.append(f"  dut.{port.emitted_name} = pyc::cpp::Wire<1>(0);\n")
        else:
            lines.append(f"  dut.{port.emitted_name} = 0;\n")
    for port in interface.resets:
        if backend == "cpp":
            lines.append(f"  dut.{port.emitted_name} = pyc::cpp::Wire<1>(0);\n")
        else:
            lines.append(f"  dut.{port.emitted_name} = 0;\n")
    for port in interface.data_inputs:
        if backend == "cpp":
            lines.append(f"  dut.{port.emitted_name} = pyc::cpp::Wire<{port.width}>();\n")
        elif port.width <= 64:
            lines.append(f"  dut.{port.emitted_name} = 0;\n")
        else:
            for index in range(port.words32):
                lines.append(f"  dut.{port.emitted_name}.at({index}) = 0;\n")
    lines.append("  dut.comb();\n" if backend == "cpp" else "  dut.eval();\n")
    lines.append("}\n")
    return "".join(lines)


def _render_clock_helpers(interface: Interface, *, backend: str) -> str:
    if interface.mode != "clocked":
        return ""
    clock = interface.clocks[0].emitted_name
    if backend == "cpp":
        return f"""inline void beginCycle(Dut &dut) {{
  dut.comb();
  dut.{clock} = pyc::cpp::Wire<1>(1);
  dut.tick();
  dut.transfer();
  dut.comb();
}}

inline void endCycle(Dut &dut) {{
  dut.{clock} = pyc::cpp::Wire<1>(0);
  dut.tick();
  dut.transfer();
}}
"""
    return f"""inline void beginCycle(Dut &dut) {{
  dut.eval();
  dut.{clock} = 1;
  dut.eval();
}}

inline void endCycle(Dut &dut) {{
  dut.{clock} = 0;
  dut.eval();
}}
"""


def _render_reset(interface: Interface, *, backend: str) -> str:
    if interface.mode != "clocked" or not interface.resets:
        return "inline void resetDut(Dut &, std::uint64_t, std::uint64_t) {}\n"
    reset = interface.resets[0]
    high = "pyc::cpp::Wire<1>(1)" if backend == "cpp" else "1"
    low = "pyc::cpp::Wire<1>(0)" if backend == "cpp" else "0"
    return f"""inline void resetDut(Dut &dut, std::uint64_t assertedCycles,
                     std::uint64_t settleCycles) {{
  dut.{reset.emitted_name} = {high};
  for (std::uint64_t i = 0; i < assertedCycles; ++i) {{ beginCycle(dut); endCycle(dut); }}
  dut.{reset.emitted_name} = {low};
  for (std::uint64_t i = 0; i < settleCycles; ++i) {{ beginCycle(dut); endCycle(dut); }}
}}
"""


def _render_run_workload(interface: Interface, *, reset_cycles: int, reset_settle_cycles: int) -> str:
    if interface.mode == "clocked":
        evaluate = "    beginCycle(dut);\n"
        finish = "    endCycle(dut);\n"
    else:
        evaluate = ""
        finish = ""
    # Backend-specific combinational calls are supplied through settleComb().
    if interface.mode == "comb":
        evaluate = "    settleComb(dut);\n"
    return f"""std::uint64_t runWorkload(Dut &dut, const InputTable &inputs,
                              std::uint64_t iterations, std::uint64_t sampleEvery,
                              bool transcript, bool sampleEveryIteration) {{
  std::uint64_t hash = 0xcbf29ce484222325ull;
  mix(hash, iterations);
  for (std::uint64_t i = 0; i < iterations; ++i) {{
    const auto &frame = inputs[static_cast<std::size_t>(i) & kInputTableMask];
    applyInputs(dut, frame);
{evaluate}    const bool sample = sampleEveryIteration || (((i + 1u) & (sampleEvery - 1u)) == 0u) || (i + 1u == iterations);
    if (sample) observe(dut, i, transcript, hash);
{finish}  }}
  return hash;
}}

int runBenchmark(int argc, char **argv, const char *backend) {{
  const std::uint64_t iterations = parseU64(argc, argv, 1, 1000000ull);
  const std::uint64_t warmupIterations = parseU64(argc, argv, 2, 10000ull);
  const std::uint64_t verifyIterations = parseU64(argc, argv, 3, 256ull);
  const std::uint64_t seed = parseU64(argc, argv, 4, 0x6a09e667f3bcc909ull);
  const std::uint64_t sampleEvery = parseU64(argc, argv, 5, 256ull);
  const bool transcript = parseU64(argc, argv, 6, 0ull) != 0ull;
  if (iterations == 0u || sampleEvery == 0u || (sampleEvery & (sampleEvery - 1u)) != 0u ||
      kInputTableSize == 0u) {{
    std::cerr << "invalid benchmark arguments\\n";
    return 2;
  }}

  const auto inputs = makeInputs(seed);
  Dut dut{{}};
  initialize(dut);
  resetDut(dut, {reset_cycles}ull, {reset_settle_cycles}ull);
  const std::uint64_t verifyDigest = runWorkload(
      dut, *inputs, verifyIterations, 1u, transcript, true);
  if (verifyIterations != 0u && {str(bool(interface.resets and interface.mode == 'clocked')).lower()})
    resetDut(dut, {reset_cycles}ull, {reset_settle_cycles}ull);
  (void)runWorkload(dut, *inputs, warmupIterations, sampleEvery, false, false);

  const auto begin = std::chrono::steady_clock::now();
  const std::uint64_t digest = runWorkload(
      dut, *inputs, iterations, sampleEvery, false, false);
  const auto end = std::chrono::steady_clock::now();
  const std::chrono::duration<double> elapsed = end - begin;

  std::uint64_t finalDigest = 0xcbf29ce484222325ull;
  observe(dut, iterations, false, finalDigest);
  emitResult(backend, {_cpp_string(interface.mode)}, iterations, elapsed.count(),
             verifyDigest, digest, finalDigest);
  finalizeDut(dut);
  return 0;
}}
"""


def render_cpp_harness(
    interface: Interface,
    *,
    top_header: str,
    reset_cycles: int,
    reset_settle_cycles: int,
) -> str:
    settle = "inline void settleComb(Dut &dut) { dut.comb(); }\n"
    finalize = "inline void finalizeDut(Dut &) {}\n"
    return f"""// Generated by pyCircuit simulator benchmark. Do not edit.
#include "bench_common.hpp"
#include {_cpp_string(top_header)}

using namespace pyc_bench;
using Dut = pyc::gen::{interface.emitted_top};

namespace {{
{_render_cpp_apply_inputs(interface)}
{_render_observe(interface, backend="cpp")}
{_render_init(interface, backend="cpp")}
{_render_clock_helpers(interface, backend="cpp")}
{_render_reset(interface, backend="cpp")}
{settle}
{finalize}
{_render_run_workload(interface, reset_cycles=reset_cycles, reset_settle_cycles=reset_settle_cycles)}
}} // namespace

int main(int argc, char **argv) {{ return runBenchmark(argc, argv, "cpp"); }}
"""


def render_verilator_harness(
    interface: Interface,
    *,
    reset_cycles: int,
    reset_settle_cycles: int,
) -> str:
    settle = "inline void settleComb(Dut &dut) { dut.eval(); }\n"
    finalize = "inline void finalizeDut(Dut &dut) { dut.final(); }\n"
    return f"""// Generated by pyCircuit simulator benchmark. Do not edit.
#include "bench_common.hpp"
#include "PYCVerilatedDut.h"
#include "verilated.h"

using namespace pyc_bench;
using Dut = PYCVerilatedDut;

namespace {{
{_render_verilator_apply_inputs(interface)}
{_render_observe(interface, backend="verilator")}
{_render_init(interface, backend="verilator")}
{_render_clock_helpers(interface, backend="verilator")}
{_render_reset(interface, backend="verilator")}
{settle}
{finalize}
{_render_run_workload(interface, reset_cycles=reset_cycles, reset_settle_cycles=reset_settle_cycles)}
}} // namespace

int main(int argc, char **argv) {{
  Verilated::commandArgs(argc, argv);
  return runBenchmark(argc, argv, "verilator");
}}
"""


def _write_if_changed(path: Path, text: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    encoded = text.encode("utf-8")
    if path.is_file() and path.read_bytes() == encoded:
        return
    temporary = path.with_suffix(path.suffix + ".tmp")
    temporary.write_bytes(encoded)
    os.replace(temporary, path)


def _load_json(path: Path) -> dict[str, Any]:
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        raise BenchmarkError(f"failed to read JSON {path}: {exc}") from exc
    if not isinstance(value, dict):
        raise BenchmarkError(f"expected a JSON object in {path}")
    return value


def _sha256_bytes(value: bytes) -> str:
    return hashlib.sha256(value).hexdigest()


def _sha256_file(path: Path) -> str:
    return _sha256_bytes(path.read_bytes())


def _tail(value: str, limit: int = 4000) -> str:
    return value if len(value) <= limit else value[-limit:]


def _run_command(
    command: Sequence[str],
    *,
    cwd: Path,
    env: Mapping[str, str],
    timeout_seconds: float,
    log_dir: Path,
    stage: str,
    cpu: int | None = None,
) -> CommandResult:
    log_dir.mkdir(parents=True, exist_ok=True)
    safe_stage = re.sub(r"[^A-Za-z0-9_.-]+", "_", stage)
    preexec_fn = None
    if cpu is not None:
        if not hasattr(os, "sched_setaffinity"):
            raise BenchmarkError("--cpu is not supported on this platform")

        def set_affinity() -> None:
            os.sched_setaffinity(0, {cpu})

        preexec_fn = set_affinity
    _write_if_changed(log_dir / f"{safe_stage}.command.txt", shlex.join(list(command)) + "\n")
    begin = time.perf_counter()
    try:
        proc = subprocess.run(
            list(command),
            cwd=str(cwd),
            env=dict(env),
            text=True,
            capture_output=True,
            timeout=timeout_seconds,
            preexec_fn=preexec_fn,
            check=False,
        )
    except subprocess.TimeoutExpired as exc:
        stdout = exc.stdout if isinstance(exc.stdout, str) else ""
        stderr = exc.stderr if isinstance(exc.stderr, str) else ""
        _write_if_changed(log_dir / f"{safe_stage}.stdout.log", stdout)
        _write_if_changed(log_dir / f"{safe_stage}.stderr.log", stderr)
        raise BenchmarkError(f"stage {stage!r} timed out after {timeout_seconds:g}s") from exc
    except (OSError, subprocess.SubprocessError) as exc:
        raise BenchmarkError(f"failed to start stage {stage!r}: {exc}") from exc
    elapsed = time.perf_counter() - begin
    _write_if_changed(log_dir / f"{safe_stage}.stdout.log", proc.stdout)
    _write_if_changed(log_dir / f"{safe_stage}.stderr.log", proc.stderr)
    if proc.returncode != 0:
        details = _tail(proc.stderr.strip() or proc.stdout.strip())
        raise BenchmarkError(
            f"stage {stage!r} failed with exit code {proc.returncode}\n"
            f"command: {shlex.join(list(command))}\n{details}"
        )
    return CommandResult(elapsed, proc.stdout, proc.stderr)


def _detect_executable(name: str, explicit: str | None = None) -> Path:
    if explicit:
        path = Path(explicit)
        if path.is_file() and os.access(path, os.X_OK):
            return path.resolve()
        found = shutil.which(explicit)
        if found:
            return Path(found).resolve()
        raise BenchmarkError(f"required executable is not available: {explicit}")
    found = shutil.which(name)
    if found:
        return Path(found).resolve()
    raise BenchmarkError(f"required executable is not available: {name}")


def _detect_pycc(project_root: Path) -> Path:
    explicit = os.environ.get("PYCC")
    if explicit:
        return _detect_executable("pycc", explicit)
    packaged = tool_executable("pycc")
    candidates = [
        packaged,
        Path(os.environ["PYC_TOOLCHAIN_ROOT"]) / "bin" / "pycc" if os.environ.get("PYC_TOOLCHAIN_ROOT") else None,
        project_root / ".pycircuit_out" / "toolchain" / "install" / "bin" / "pycc",
        Path(__file__).resolve().parents[3] / ".pycircuit_out" / "toolchain" / "install" / "bin" / "pycc",
    ]
    for candidate in candidates:
        if candidate is not None and candidate.is_file() and os.access(candidate, os.X_OK):
            return candidate.resolve()
    return _detect_executable("pycc")


def _prepend_pythonpath(env: dict[str, str], paths: Iterable[Path]) -> None:
    values: list[str] = []
    seen: set[str] = set()
    for path in paths:
        value = str(path.resolve())
        if value not in seen:
            seen.add(value)
            values.append(value)
    old = env.get("PYTHONPATH")
    if old:
        values.append(old)
    env["PYTHONPATH"] = os.pathsep.join(values)


def _resolve_manifest_path(base: Path, value: str) -> Path:
    path = Path(value)
    return path.resolve() if path.is_absolute() else (base / path).resolve()


def _aggregate_cpp_manifests(paths: Sequence[Path]) -> dict[str, Any]:
    sources: list[Path] = []
    include_dirs: list[Path] = []
    runtime_sources: list[Path] = []
    runtime_include_dirs: list[Path] = []
    runtime_library_files: list[Path] = []
    runtime_lib_dirs: list[Path] = []
    runtime_libs: list[str] = []
    compile_defines: list[str] = []
    runtime_cmake_config_dir: Path | None = None
    runtime_cmake_package = "pycircuit"
    runtime_cmake_target = "pycircuit::pyc4_runtime"
    top_headers: dict[str, str] = {}

    def add_unique(items: list[Any], value: Any) -> None:
        if value not in items:
            items.append(value)

    for path in paths:
        data = _load_json(path)
        base = path.parent
        top_headers[str(data.get("target_name", ""))] = str(data.get("top_header", ""))
        for entry in data.get("sources", []):
            if isinstance(entry, Mapping) and isinstance(entry.get("path"), str):
                add_unique(sources, _resolve_manifest_path(base, str(entry["path"])))
        for value in data.get("include_dirs", []):
            if isinstance(value, str):
                add_unique(include_dirs, _resolve_manifest_path(base, value))
        for value in data.get("runtime_sources", []):
            if isinstance(value, str):
                add_unique(runtime_sources, _resolve_manifest_path(base, value))
        for value in data.get("runtime_include_dirs", []):
            if isinstance(value, str):
                add_unique(runtime_include_dirs, _resolve_manifest_path(base, value))
        for value in data.get("compile_defines", []):
            if isinstance(value, str) and value:
                add_unique(compile_defines, value)
        runtime = data.get("runtime", {})
        if not isinstance(runtime, Mapping):
            continue
        for value in runtime.get("include_dirs", []):
            if isinstance(value, str):
                add_unique(runtime_include_dirs, _resolve_manifest_path(base, value))
        for value in runtime.get("library_files", []):
            if isinstance(value, str):
                add_unique(runtime_library_files, _resolve_manifest_path(base, value))
        for value in runtime.get("lib_dirs", []):
            if isinstance(value, str):
                add_unique(runtime_lib_dirs, _resolve_manifest_path(base, value))
        for value in runtime.get("libs", []):
            if isinstance(value, str) and value:
                add_unique(runtime_libs, value)
        config_value = runtime.get("cmake_config_dir")
        if runtime_cmake_config_dir is None and isinstance(config_value, str) and config_value:
            runtime_cmake_config_dir = _resolve_manifest_path(base, config_value)
        runtime_cmake_package = str(runtime.get("cmake_package", runtime_cmake_package))
        runtime_cmake_target = str(runtime.get("cmake_target", runtime_cmake_target))

    for path in [*sources, *runtime_sources, *runtime_library_files]:
        if not path.is_file():
            raise BenchmarkError(f"C++ compile manifest references a missing file: {path}")
    if not runtime_include_dirs:
        raise BenchmarkError(
            "C++ compile manifests contain no runtime include directory; "
            "set PYC_TOOLCHAIN_ROOT or use the bundled/staged pycc"
        )
    config_file = (
        runtime_cmake_config_dir / f"{runtime_cmake_package}Config.cmake"
        if runtime_cmake_config_dir is not None
        else None
    )
    if not (
        (config_file is not None and config_file.is_file())
        or runtime_library_files
        or runtime_sources
        or (runtime_lib_dirs and runtime_libs)
    ):
        raise BenchmarkError(
            "C++ compile manifests contain no usable runtime library; "
            "set PYC_TOOLCHAIN_ROOT or use the bundled/staged pycc"
        )
    return {
        "sources": sources,
        "include_dirs": include_dirs,
        "runtime_sources": runtime_sources,
        "runtime_include_dirs": runtime_include_dirs,
        "runtime_library_files": runtime_library_files,
        "runtime_lib_dirs": runtime_lib_dirs,
        "runtime_libs": runtime_libs,
        "compile_defines": compile_defines,
        "runtime_cmake_config_dir": runtime_cmake_config_dir,
        "runtime_cmake_package": runtime_cmake_package,
        "runtime_cmake_target": runtime_cmake_target,
        "top_headers": top_headers,
    }


def _cmake_quote(value: str | Path) -> str:
    return str(value).replace("\\", "/").replace('"', '\\"')


def _render_cmake_project(
    aggregate: Mapping[str, Any],
    *,
    harness: Path,
    output_dir: Path,
    native: bool,
) -> str:
    sources = [*aggregate["sources"], *aggregate["runtime_sources"], harness]
    includes = [*aggregate["include_dirs"], *aggregate["runtime_include_dirs"], harness.parent]
    flags = ["-O3", "-DNDEBUG", "-fno-lto"] + (["-march=native"] if native else [])
    lines = [
        "cmake_minimum_required(VERSION 3.20)\n",
        "project(pyc_sim_benchmark LANGUAGES CXX)\n",
        "set(CMAKE_CXX_STANDARD 17)\n",
        "set(CMAKE_CXX_STANDARD_REQUIRED ON)\n",
        "set(CMAKE_CXX_EXTENSIONS OFF)\n",
        "add_executable(pyc_cpp_bench\n",
    ]
    lines.extend(f'  "{_cmake_quote(path)}"\n' for path in sources)
    lines.append(")\n")
    lines.append("target_include_directories(pyc_cpp_bench PRIVATE\n")
    lines.extend(f'  "{_cmake_quote(path)}"\n' for path in includes)
    lines.append(")\n")
    if aggregate["compile_defines"]:
        lines.append("target_compile_definitions(pyc_cpp_bench PRIVATE\n")
        lines.extend(f"  {value}\n" for value in aggregate["compile_defines"])
        lines.append(")\n")
    lines.append("if(CMAKE_CXX_COMPILER_ID MATCHES \"GNU|Clang\")\n")
    lines.append("  target_compile_options(pyc_cpp_bench PRIVATE " + " ".join(flags) + ")\n")
    lines.append("  target_link_options(pyc_cpp_bench PRIVATE -fno-lto)\n")
    lines.append("endif()\n")
    config_dir = aggregate["runtime_cmake_config_dir"]
    package = aggregate["runtime_cmake_package"]
    target = aggregate["runtime_cmake_target"]
    config_file = Path(config_dir) / f"{package}Config.cmake" if config_dir else None
    library_files = aggregate["runtime_library_files"]
    if config_file is not None and config_file.is_file():
        lines.append(f'find_package({package} CONFIG REQUIRED PATHS "{_cmake_quote(config_dir)}" NO_DEFAULT_PATH)\n')
        lines.append(f"target_link_libraries(pyc_cpp_bench PRIVATE {target})\n")
    elif library_files:
        lines.append("target_link_libraries(pyc_cpp_bench PRIVATE\n")
        lines.extend(f'  "{_cmake_quote(path)}"\n' for path in library_files)
        lines.append(")\n")
    elif aggregate["runtime_sources"]:
        # The runtime sources are already part of this executable.
        pass
    elif aggregate["runtime_libs"]:
        if aggregate["runtime_lib_dirs"]:
            lines.append("target_link_directories(pyc_cpp_bench PRIVATE\n")
            lines.extend(f'  "{_cmake_quote(path)}"\n' for path in aggregate["runtime_lib_dirs"])
            lines.append(")\n")
        lines.append("target_link_libraries(pyc_cpp_bench PRIVATE " + " ".join(aggregate["runtime_libs"]) + ")\n")
    lines.append(f'set_target_properties(pyc_cpp_bench PROPERTIES RUNTIME_OUTPUT_DIRECTORY "{_cmake_quote(output_dir)}")\n')
    return "".join(lines)


def parse_native_output(output: str, *, external_wall_seconds: float) -> tuple[NativeResult, list[str]]:
    observations = [line.strip() for line in output.splitlines() if line.startswith(OBS_PREFIX)]
    result_lines = [line[len(RESULT_PREFIX) :].strip() for line in output.splitlines() if line.startswith(RESULT_PREFIX)]
    if len(result_lines) != 1:
        raise BenchmarkError(f"native benchmark emitted {len(result_lines)} result markers; expected exactly one")
    try:
        payload = json.loads(result_lines[0])
    except json.JSONDecodeError as exc:
        raise BenchmarkError(f"invalid native benchmark result JSON: {exc}") from exc
    if not isinstance(payload, dict):
        raise BenchmarkError("native benchmark result must be a JSON object")
    required = {
        "schema_version",
        "backend",
        "mode",
        "iterations",
        "seconds",
        "throughput_per_second",
        "verify_digest",
        "digest",
        "final_digest",
    }
    missing = sorted(required - set(payload))
    if missing:
        raise BenchmarkError(f"native benchmark result is missing: {', '.join(missing)}")
    if payload["schema_version"] != RESULT_SCHEMA_VERSION:
        raise BenchmarkError(
            f"native benchmark schema mismatch: {payload['schema_version']!r}; "
            f"expected {RESULT_SCHEMA_VERSION}"
        )
    try:
        iterations = int(payload["iterations"])
        seconds = float(payload["seconds"])
        throughput = float(payload["throughput_per_second"])
    except (TypeError, ValueError) as exc:
        raise BenchmarkError(f"native benchmark reported malformed numeric fields: {exc}") from exc
    if iterations < 0:
        raise BenchmarkError(f"native benchmark reported invalid iteration count: {iterations}")
    if not math.isfinite(seconds) or seconds <= 0.0:
        raise BenchmarkError(f"native benchmark reported invalid duration: {seconds!r}")
    if not math.isfinite(throughput) or throughput <= 0.0:
        raise BenchmarkError(f"native benchmark reported invalid throughput: {throughput!r}")
    return (
        NativeResult(
            backend=str(payload["backend"]),
            mode=str(payload["mode"]),
            iterations=iterations,
            seconds=seconds,
            throughput_per_second=throughput,
            verify_digest=str(payload["verify_digest"]),
            digest=str(payload["digest"]),
            final_digest=str(payload["final_digest"]),
            external_wall_seconds=float(external_wall_seconds),
        ),
        observations,
    )


def compare_observations(cpp: Sequence[str], verilator: Sequence[str]) -> None:
    common = min(len(cpp), len(verilator))
    for index in range(common):
        if cpp[index] != verilator[index]:
            raise BenchmarkError(
                "cross-backend verification failed at observation "
                f"{index}:\n  C++:       {cpp[index]}\n  Verilator: {verilator[index]}"
            )
    if len(cpp) != len(verilator):
        raise BenchmarkError(
            "cross-backend verification produced different transcript lengths: "
            f"C++={len(cpp)}, Verilator={len(verilator)}"
        )


def _percentile(values: Sequence[float], fraction: float) -> float:
    ordered = sorted(float(value) for value in values)
    if not ordered:
        raise ValueError("percentile requires at least one value")
    if len(ordered) == 1:
        return ordered[0]
    position = (len(ordered) - 1) * fraction
    lower = math.floor(position)
    upper = math.ceil(position)
    if lower == upper:
        return ordered[lower]
    weight = position - lower
    return ordered[lower] * (1.0 - weight) + ordered[upper] * weight


def summarize_samples(samples: Sequence[NativeResult]) -> dict[str, float | int]:
    if not samples:
        raise BenchmarkError("cannot summarize an empty sample set")
    seconds = [sample.seconds for sample in samples]
    throughput = [sample.throughput_per_second for sample in samples]
    median_seconds = statistics.median(seconds)
    median_throughput = statistics.median(throughput)
    deviations = [abs(value - median_throughput) for value in throughput]
    return {
        "count": len(samples),
        "median_seconds": median_seconds,
        "min_seconds": min(seconds),
        "max_seconds": max(seconds),
        "median_throughput_per_second": median_throughput,
        "min_throughput_per_second": min(throughput),
        "max_throughput_per_second": max(throughput),
        "p25_throughput_per_second": _percentile(throughput, 0.25),
        "p75_throughput_per_second": _percentile(throughput, 0.75),
        "mad_throughput_per_second": statistics.median(deviations),
    }


def _native_result_dict(result: NativeResult) -> dict[str, Any]:
    return {
        "backend": result.backend,
        "mode": result.mode,
        "iterations": result.iterations,
        "seconds": result.seconds,
        "throughput_per_second": result.throughput_per_second,
        "verify_digest": result.verify_digest,
        "digest": result.digest,
        "final_digest": result.final_digest,
        "external_wall_seconds": result.external_wall_seconds,
    }


def _tool_version(command: Sequence[str], *, cwd: Path, env: Mapping[str, str]) -> str:
    try:
        proc = subprocess.run(list(command), cwd=str(cwd), env=dict(env), text=True, capture_output=True, timeout=10)
    except (OSError, subprocess.SubprocessError):
        return "unknown"
    value = (proc.stdout.strip() or proc.stderr.strip()).splitlines()
    return value[0] if value else "unknown"


def _git_metadata(root: Path) -> dict[str, Any]:
    def query(args: Sequence[str]) -> str:
        try:
            proc = subprocess.run(["git", *args], cwd=str(root), text=True, capture_output=True, timeout=10)
        except (OSError, subprocess.SubprocessError):
            return ""
        return proc.stdout.strip() if proc.returncode == 0 else ""

    return {
        "commit": query(["rev-parse", "HEAD"]),
        "branch": query(["rev-parse", "--abbrev-ref", "HEAD"]),
        "dirty": bool(query(["status", "--porcelain"])),
    }


def _cpu_model() -> str:
    path = Path("/proc/cpuinfo")
    if path.is_file():
        for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
            if line.lower().startswith("model name") and ":" in line:
                return line.split(":", 1)[1].strip()
    return platform.processor() or "unknown"


def _validate_args(args: argparse.Namespace) -> None:
    positive = {
        "--iterations": args.iterations,
        "--verify-iterations": args.verify_iterations,
        "--repeats": args.repeats,
        "--input-table-size": args.input_table_size,
        "--sample-every": args.sample_every,
        "--jobs": args.jobs,
        "--logic-depth": args.logic_depth,
        "--max-port-bits": args.max_port_bits,
    }
    for name, value in positive.items():
        if int(value) <= 0:
            raise BenchmarkError(f"{name} must be > 0")
    for name in ("warmup_iterations", "reset_cycles", "reset_settle_cycles"):
        if int(getattr(args, name)) < 0:
            raise BenchmarkError(f"--{name.replace('_', '-')} must be >= 0")
    u64_values = {
        "--iterations": args.iterations,
        "--warmup-iterations": args.warmup_iterations,
        "--verify-iterations": args.verify_iterations,
        "--sample-every": args.sample_every,
        "--reset-cycles": args.reset_cycles,
        "--reset-settle-cycles": args.reset_settle_cycles,
    }
    for name, value in u64_values.items():
        if int(value) > (1 << 64) - 1:
            raise BenchmarkError(f"{name} must fit in an unsigned 64-bit integer")
    if float(args.timeout_seconds) <= 0.0:
        raise BenchmarkError("--timeout-seconds must be > 0")
    if float(args.min_sample_seconds) < 0.0:
        raise BenchmarkError("--min-sample-seconds must be >= 0")
    if args.cpu is not None and int(args.cpu) < 0:
        raise BenchmarkError("--cpu must be >= 0")
    if args.cpu is not None and hasattr(os, "sched_getaffinity"):
        allowed_cpus = os.sched_getaffinity(0)
        if int(args.cpu) not in allowed_cpus:
            allowed_text = ",".join(str(cpu) for cpu in sorted(allowed_cpus))
            raise BenchmarkError(f"--cpu={args.cpu} is outside the allowed CPU set: {allowed_text}")
    if not 0 <= int(args.seed) <= (1 << 64) - 1:
        raise BenchmarkError("--seed must fit in an unsigned 64-bit integer")
    if int(args.input_table_size) & (int(args.input_table_size) - 1):
        raise BenchmarkError("--input-table-size must be a power of two")
    if int(args.input_table_size) > MAX_INPUT_TABLE_BYTES:
        raise BenchmarkError("--input-table-size exceeds the 256 MiB input-table safety limit")
    if int(args.sample_every) & (int(args.sample_every) - 1):
        raise BenchmarkError("--sample-every must be a power of two")


def _benchmark_env(base: Mapping[str, str]) -> dict[str, str]:
    env = dict(base)
    for name in ("PYC_TRACE_DIR", "PYC_SIM_STATS_PATH"):
        env.pop(name, None)
    env["PYC_SIM_STATS"] = "0"
    env["PYC_SIM_FAST"] = "0"
    env["PYC_KONATA"] = "0"
    return env


def _run_native(
    executable: Path,
    *,
    backend: str,
    iterations: int,
    warmup_iterations: int,
    verify_iterations: int,
    seed: int,
    sample_every: int,
    transcript: bool,
    cwd: Path,
    env: Mapping[str, str],
    timeout_seconds: float,
    log_dir: Path,
    stage: str,
    cpu: int | None,
) -> tuple[NativeResult, list[str]]:
    command = [
        str(executable),
        str(iterations),
        str(warmup_iterations),
        str(verify_iterations),
        hex(seed),
        str(sample_every),
        "1" if transcript else "0",
    ]
    result = _run_command(
        command,
        cwd=cwd,
        env=env,
        timeout_seconds=timeout_seconds,
        log_dir=log_dir,
        stage=stage,
        cpu=cpu,
    )
    native, observations = parse_native_output(result.stdout, external_wall_seconds=result.wall_seconds)
    if native.backend != backend:
        raise BenchmarkError(f"{backend} executable reported backend={native.backend!r}")
    if native.iterations != iterations:
        raise BenchmarkError(f"{backend} executable ran {native.iterations} iterations, expected {iterations}")
    return native, observations


def run_benchmark(args: argparse.Namespace) -> dict[str, Any]:
    _validate_args(args)
    source = Path(args.python_file).resolve()
    if not source.is_file():
        raise BenchmarkError(f"benchmark source does not exist: {source}")
    project_root = Path(args.project_root).resolve() if args.project_root else nearest_project_root(source)
    out_dir = (
        Path(args.out_dir).resolve()
        if args.out_dir
        else (project_root / ".pycircuit_out" / "perf" / "cpp-vs-verilator" / source.stem).resolve()
    )
    if any(character.isspace() for character in str(out_dir)):
        raise BenchmarkError(
            "--out-dir must not contain whitespace because Verilator's generated Makefile "
            "does not preserve whitespace in --Mdir or user source paths"
        )
    out_dir.mkdir(parents=True, exist_ok=True)
    log_dir = out_dir / "logs"
    generated_dir = out_dir / "generated"
    canonical_dir = out_dir / "canonical"
    cpp_gen_dir = generated_dir / "cpp"
    verilog_harness_dir = generated_dir / "harness"
    build_dir = out_dir / "native"

    pycc = _detect_pycc(project_root)
    verilator = _detect_executable("verilator", args.verilator)
    cmake = _detect_executable("cmake", args.cmake)
    cxx = _detect_executable(args.cxx, args.cxx)

    env = os.environ.copy()
    env["PYCC"] = str(pycc)
    env["PYTHONDONTWRITEBYTECODE"] = "1"
    _prepend_pythonpath(
        env,
        [Path(__file__).resolve().parent.parent, source.parent, project_root],
    )
    build_env = dict(env)
    # Build timing should not depend on a warm compiler cache, and disabling it
    # also keeps Verilator usable in restricted/readonly home directories.
    build_env["CCACHE_DISABLE"] = "1"

    print(f"[benchmark] frontend + Verilog emit: {source}")
    frontend_command = [
        sys.executable,
        "-m",
        "pycircuit.cli",
        "build",
        str(source),
        "--out-dir",
        str(canonical_dir),
        "--target",
        "verilator",
        "--profile",
        "release",
        "--jobs",
        str(args.jobs),
        "--logic-depth",
        str(args.logic_depth),
        "--project-root",
        str(project_root),
    ]
    for value in args.param:
        frontend_command.extend(["--param", value])
    frontend_result = _run_command(
        frontend_command,
        cwd=project_root,
        env=build_env,
        timeout_seconds=args.timeout_seconds,
        log_dir=log_dir,
        stage="frontend_verilog_emit",
    )

    project_manifest_path = canonical_dir / "project_manifest.json"
    project_manifest = _load_json(project_manifest_path)
    interface = load_interface(
        project_manifest,
        requested_mode=args.mode,
        max_port_bits=args.max_port_bits,
    )
    input_frame_bytes = max(1, sum(port.words32 * 4 for port in interface.data_inputs))
    input_table_bytes = input_frame_bytes * args.input_table_size
    if input_table_bytes > MAX_INPUT_TABLE_BYTES:
        raise BenchmarkError(
            f"input table would require {input_table_bytes} bytes, exceeding the "
            f"{MAX_INPUT_TABLE_BYTES}-byte safety limit; reduce --input-table-size"
        )
    module_rows = project_manifest.get("modules", [])
    if not isinstance(module_rows, list) or not module_rows:
        raise BenchmarkError("project manifest contains no modules")

    print(f"[benchmark] C++ emit: {len(module_rows)} module(s)")
    cpp_manifest_paths: list[Path] = []
    cpp_emit_seconds = 0.0
    probe_plan_path = canonical_dir / "probe_plan.json"
    for row in sorted((row for row in module_rows if isinstance(row, Mapping)), key=lambda row: str(row.get("name", ""))):
        symbol = str(row.get("name", "")).strip()
        pyc_value = str(row.get("pyc", "")).strip()
        if not symbol or not pyc_value:
            raise BenchmarkError("module manifest row is missing name/pyc")
        pyc_path = _resolve_manifest_path(canonical_dir, pyc_value)
        module_out = cpp_gen_dir / symbol
        command = [
            str(pycc),
            str(pyc_path),
            "--emit=cpp",
            "--build-profile=release",
            "--inline-policy=off",
            "--hierarchy-policy=strict",
            "--out-dir",
            str(module_out),
            "--cpp-split=module",
            f"--logic-depth={args.logic_depth}",
        ]
        if probe_plan_path.is_file():
            command.extend(["--probe-plan", str(probe_plan_path)])
        result = _run_command(
            command,
            cwd=project_root,
            env=build_env,
            timeout_seconds=args.timeout_seconds,
            log_dir=log_dir,
            stage=f"cpp_emit_{symbol}",
        )
        cpp_emit_seconds += result.wall_seconds
        manifest_path = module_out / "cpp_compile_manifest.json"
        if not manifest_path.is_file():
            raise BenchmarkError(f"pycc did not emit C++ compile manifest for {symbol}: {manifest_path}")
        cpp_manifest_paths.append(manifest_path)

    aggregate = _aggregate_cpp_manifests(cpp_manifest_paths)
    top_header = aggregate["top_headers"].get(interface.top)
    if not top_header:
        raise BenchmarkError(f"C++ compile manifests do not identify the top header for {interface.top!r}")

    common_header = verilog_harness_dir / "bench_common.hpp"
    cpp_harness = verilog_harness_dir / "cpp_bench.cpp"
    verilator_harness = verilog_harness_dir / "verilator_bench.cpp"
    _write_if_changed(
        common_header,
        render_common_harness_header(interface, input_table_size=args.input_table_size),
    )
    _write_if_changed(
        cpp_harness,
        render_cpp_harness(
            interface,
            top_header=top_header,
            reset_cycles=args.reset_cycles,
            reset_settle_cycles=args.reset_settle_cycles,
        ),
    )
    _write_if_changed(
        verilator_harness,
        render_verilator_harness(
            interface,
            reset_cycles=args.reset_cycles,
            reset_settle_cycles=args.reset_settle_cycles,
        ),
    )

    print("[benchmark] native build: pyCircuit C++")
    cpp_cmake_source = build_dir / "cpp_source"
    cpp_cmake_build = build_dir / "cpp_build"
    cpp_binary_dir = build_dir / "bin"
    cmake_text = _render_cmake_project(
        aggregate,
        harness=cpp_harness,
        output_dir=cpp_binary_dir,
        native=bool(args.native),
    )
    _write_if_changed(cpp_cmake_source / "CMakeLists.txt", cmake_text)
    cmake_generator = "Ninja" if shutil.which("ninja") else "Unix Makefiles"
    configure_result = _run_command(
        [
            str(cmake),
            "-G",
            cmake_generator,
            "-S",
            str(cpp_cmake_source),
            "-B",
            str(cpp_cmake_build),
            "-DCMAKE_BUILD_TYPE=Release",
            f"-DCMAKE_CXX_COMPILER={cxx}",
        ],
        cwd=project_root,
        env=build_env,
        timeout_seconds=args.timeout_seconds,
        log_dir=log_dir,
        stage="cpp_cmake_configure",
    )
    cpp_build_result = _run_command(
        [str(cmake), "--build", str(cpp_cmake_build), "-j", str(args.jobs)],
        cwd=project_root,
        env=build_env,
        timeout_seconds=args.timeout_seconds,
        log_dir=log_dir,
        stage="cpp_native_build",
    )
    cpp_executable = cpp_binary_dir / ("pyc_cpp_bench.exe" if os.name == "nt" else "pyc_cpp_bench")
    if not cpp_executable.is_file():
        raise BenchmarkError(f"missing C++ benchmark executable: {cpp_executable}")

    print("[benchmark] native build: Verilator")
    verilator_manifest_value = str(project_manifest.get("verilator_manifest", "verilator_manifest.json"))
    verilator_manifest = _load_json(_resolve_manifest_path(canonical_dir, verilator_manifest_value))
    verilog_sources = [
        _resolve_manifest_path(canonical_dir, value)
        for value in verilator_manifest.get("sources", [])
        if isinstance(value, str) and value
    ]
    if not verilog_sources or any(not path.is_file() for path in verilog_sources):
        raise BenchmarkError("Verilator manifest contains missing or empty DUT sources")
    verilog_include_dirs = [
        _resolve_manifest_path(canonical_dir, value)
        for value in verilator_manifest.get("include_dirs", [])
        if isinstance(value, str) and value
    ]
    if any(not path.is_dir() for path in verilog_include_dirs):
        raise BenchmarkError("Verilator manifest references a missing include directory")
    verilator_obj_dir = build_dir / "verilator"
    common_flags = ["-O3", "-DNDEBUG", "-fno-lto"] + (["-march=native"] if args.native else [])
    # Verilator's generated makefile appends OPT_FAST after -CFLAGS when it
    # compiles a user harness.  Override all optimization buckets so its final
    # flag cannot silently downgrade the benchmark loop to -Os.  `-B` also
    # prevents stale objects when flags or --cxx change in a reused out-dir.
    verilator_make_flags = "-B OPT_FAST=-O3 OPT_SLOW=-O3 OPT_GLOBAL=-O3"
    verilator_command = [
        str(verilator),
        "--cc",
        "--exe",
        "--build",
        "--no-timing",
        "--threads",
        "1",
        "-j",
        str(args.jobs),
        "-O3",
        "-Wall",
        "-Wno-fatal",
        "-Wno-DECLFILENAME",
        "-Wno-UNUSEDSIGNAL",
        "-Wno-WIDTHEXPAND",
        "--x-initial",
        "0",
        "--x-assign",
        "0",
        "--top-module",
        interface.emitted_top,
        "--prefix",
        "PYCVerilatedDut",
        "--Mdir",
        str(verilator_obj_dir),
        "-o",
        "pyc_verilator_bench",
        "-CFLAGS",
        " ".join([*common_flags, f"-I{shlex.quote(str(verilog_harness_dir))}"]),
        "-MAKEFLAGS",
        verilator_make_flags,
        *[f"-I{path}" for path in verilog_include_dirs],
        str(verilator_harness),
        *map(str, verilog_sources),
    ]
    verilator_env = dict(build_env)
    verilator_env["CXX"] = str(cxx)
    verilator_build_result = _run_command(
        verilator_command,
        cwd=project_root,
        env=verilator_env,
        timeout_seconds=args.timeout_seconds,
        log_dir=log_dir,
        stage="verilator_native_build",
    )
    verilator_executable = verilator_obj_dir / ("pyc_verilator_bench.exe" if os.name == "nt" else "pyc_verilator_bench")
    if not verilator_executable.is_file():
        raise BenchmarkError(f"missing Verilator benchmark executable: {verilator_executable}")

    run_env = _benchmark_env(env)
    print(f"[benchmark] exact preflight: {args.verify_iterations} {interface.operation}")
    cpp_preflight, cpp_observations = _run_native(
        cpp_executable,
        backend="cpp",
        iterations=1,
        warmup_iterations=0,
        verify_iterations=args.verify_iterations,
        seed=args.seed,
        sample_every=1,
        transcript=True,
        cwd=out_dir,
        env=run_env,
        timeout_seconds=args.timeout_seconds,
        log_dir=log_dir,
        stage="preflight_cpp",
        cpu=args.cpu,
    )
    verilator_preflight, verilator_observations = _run_native(
        verilator_executable,
        backend="verilator",
        iterations=1,
        warmup_iterations=0,
        verify_iterations=args.verify_iterations,
        seed=args.seed,
        sample_every=1,
        transcript=True,
        cwd=out_dir,
        env=run_env,
        timeout_seconds=args.timeout_seconds,
        log_dir=log_dir,
        stage="preflight_verilator",
        cpu=args.cpu,
    )
    compare_observations(cpp_observations, verilator_observations)
    expected_observations = args.verify_iterations * sum(port.words64 for port in interface.outputs)
    if len(cpp_observations) != expected_observations:
        raise BenchmarkError(
            "preflight emitted an unexpected observation count: "
            f"got {len(cpp_observations)}, expected {expected_observations}"
        )
    if cpp_preflight.verify_digest != verilator_preflight.verify_digest:
        raise BenchmarkError(
            "cross-backend verification digest mismatch: "
            f"C++={cpp_preflight.verify_digest}, Verilator={verilator_preflight.verify_digest}"
        )
    if (
        cpp_preflight.digest != verilator_preflight.digest
        or cpp_preflight.final_digest != verilator_preflight.final_digest
    ):
        raise BenchmarkError(
            "cross-backend post-preflight digest mismatch: "
            f"C++={cpp_preflight.digest}/{cpp_preflight.final_digest}, "
            f"Verilator={verilator_preflight.digest}/{verilator_preflight.final_digest}"
        )
    transcript_blob = ("\n".join(cpp_observations) + "\n").encode("utf-8")
    transcript_sha256 = _sha256_bytes(transcript_blob)

    print(
        f"[benchmark] measure: {args.repeats} repeat(s), "
        f"{args.iterations} {interface.operation}/sample"
    )
    samples: dict[str, list[NativeResult]] = {"cpp": [], "verilator": []}
    for repeat in range(args.repeats):
        order = ("cpp", "verilator") if repeat % 2 == 0 else ("verilator", "cpp")
        pair: dict[str, NativeResult] = {}
        for backend in order:
            executable = cpp_executable if backend == "cpp" else verilator_executable
            native, observations = _run_native(
                executable,
                backend=backend,
                iterations=args.iterations,
                warmup_iterations=args.warmup_iterations,
                verify_iterations=0,
                seed=args.seed,
                sample_every=args.sample_every,
                transcript=False,
                cwd=out_dir,
                env=run_env,
                timeout_seconds=args.timeout_seconds,
                log_dir=log_dir,
                stage=f"measure_{repeat + 1:02d}_{backend}",
                cpu=args.cpu,
            )
            if observations:
                raise BenchmarkError(f"{backend} emitted unexpected timed-run observations")
            pair[backend] = native
            samples[backend].append(native)
        if pair["cpp"].digest != pair["verilator"].digest or pair["cpp"].final_digest != pair["verilator"].final_digest:
            raise BenchmarkError(
                f"timed-run digest mismatch in repeat {repeat + 1}: "
                f"C++={pair['cpp'].digest}/{pair['cpp'].final_digest}, "
                f"Verilator={pair['verilator'].digest}/{pair['verilator'].final_digest}"
            )

    for backend in ("cpp", "verilator"):
        digests = {(sample.digest, sample.final_digest) for sample in samples[backend]}
        if len(digests) != 1:
            raise BenchmarkError(f"{backend} produced non-deterministic digests across repeats")

    cpp_stats = summarize_samples(samples["cpp"])
    verilator_stats = summarize_samples(samples["verilator"])
    cpp_throughput = float(cpp_stats["median_throughput_per_second"])
    verilator_throughput = float(verilator_stats["median_throughput_per_second"])
    ratio = cpp_throughput / verilator_throughput
    warnings: list[str] = []
    shortest = min(sample.seconds for backend_samples in samples.values() for sample in backend_samples)
    if shortest < args.min_sample_seconds:
        warnings.append(
            f"shortest timed sample was {shortest:.6f}s (< {args.min_sample_seconds:.6f}s); "
            "increase --iterations for more stable measurements"
        )

    design_pyc = _resolve_manifest_path(canonical_dir, str(project_manifest.get("design_pyc", "device/design.pyc")))
    module_hashes = {
        str(row["name"]): _sha256_file(_resolve_manifest_path(canonical_dir, str(row["pyc"])))
        for row in module_rows
        if isinstance(row, Mapping) and "name" in row and "pyc" in row
    }
    report = {
        "schema_version": RESULT_SCHEMA_VERSION,
        "status": "pass",
        "timestamp_utc": datetime.now(timezone.utc).isoformat(),
        "case": {
            "source": str(source),
            "source_sha256": _sha256_file(source),
            "top": interface.top,
            "mode": interface.mode,
            "operation": interface.operation,
            "observation": interface.observation,
            "parameters": list(args.param),
            "design_pyc_sha256": _sha256_file(design_pyc),
            "module_pyc_sha256": module_hashes,
        },
        "contract": {
            "decisions": ["0015", "0112", "0113", "0121"],
            "same_elaboration": True,
            "trace": "off",
            "threads": 1,
            "value_domain": "2-state top-level port transcript",
            "workload_scope": "public-port drive + DUT evaluation + sampled output digest",
        },
        "workload": {
            "iterations": args.iterations,
            "warmup_iterations": args.warmup_iterations,
            "verify_iterations": args.verify_iterations,
            "repeats": args.repeats,
            "seed": hex(args.seed),
            "input_table_size": args.input_table_size,
            "input_frame_bytes": input_frame_bytes,
            "input_table_bytes": input_table_bytes,
            "sample_every": args.sample_every,
            "activity": "all-data-inputs-randomized-every-iteration",
            "reset_cycles": args.reset_cycles,
            "reset_settle_cycles": args.reset_settle_cycles,
            "cpu_affinity": args.cpu,
        },
        "interface": {
            "inputs": [port.__dict__ for port in interface.inputs],
            "outputs": [port.__dict__ for port in interface.outputs],
        },
        "correctness": {
            "status": "pass",
            "method": "exact ordered top-level port-word transcript",
            "scope": "exact preflight; timed runs compare sampled and final digests",
            "observation": interface.observation,
            "observations": len(cpp_observations),
            "transcript_sha256": transcript_sha256,
            "verify_digest": cpp_preflight.verify_digest,
            "limitations": [
                "top-level comparison is 2-state; X/Z masks are not exposed by the Verilator public-port ABI",
                "internal state is covered only through its effect on observed top-level outputs",
                "timed-run outputs are sampled at --sample-every; exact transcript comparison covers only preflight",
            ],
        },
        "build": {
            "frontend_verilog_emit_seconds": frontend_result.wall_seconds,
            "frontend_cache_hit": "jit-cache: hit" in frontend_result.stdout,
            "cpp_emit_seconds": cpp_emit_seconds,
            "cpp_cmake_configure_seconds": configure_result.wall_seconds,
            "cpp_native_build_seconds": cpp_build_result.wall_seconds,
            "verilator_translate_and_build_seconds": verilator_build_result.wall_seconds,
            "flags": common_flags,
            "verilator_make_flags": verilator_make_flags,
            "cmake_generator": cmake_generator,
        },
        "runs": {
            "cpp": {
                "samples": [_native_result_dict(sample) for sample in samples["cpp"]],
                "stats": cpp_stats,
            },
            "verilator": {
                "samples": [_native_result_dict(sample) for sample in samples["verilator"]],
                "stats": verilator_stats,
            },
        },
        "comparison": {
            "cpp_over_verilator_throughput": ratio,
            "faster_backend": "cpp" if ratio >= 1.0 else "verilator",
            "speedup": ratio if ratio >= 1.0 else (1.0 / ratio),
        },
        "environment": {
            "platform": platform.platform(),
            "machine": platform.machine(),
            "cpu_model": _cpu_model(),
            "logical_cpu_count": os.cpu_count(),
            "python": sys.version.splitlines()[0],
            "git": _git_metadata(project_root),
            "tools": {
                "pycc": {"path": str(pycc), "version": _tool_version([str(pycc), "--version"], cwd=project_root, env=env)},
                "verilator": {
                    "path": str(verilator),
                    "version": _tool_version([str(verilator), "--version"], cwd=project_root, env=env),
                },
                "cmake": {"path": str(cmake), "version": _tool_version([str(cmake), "--version"], cwd=project_root, env=env)},
                "cxx": {"path": str(cxx), "version": _tool_version([str(cxx), "--version"], cwd=project_root, env=env)},
            },
            "benchmark_env": {"PYC_SIM_STATS": "0", "PYC_SIM_FAST": "0", "trace": "off"},
        },
        "artifacts": {
            "out_dir": str(out_dir),
            "project_manifest": str(project_manifest_path),
            "cpp_executable": str(cpp_executable),
            "verilator_executable": str(verilator_executable),
            "cpp_executable_bytes": cpp_executable.stat().st_size,
            "verilator_executable_bytes": verilator_executable.stat().st_size,
            "logs": str(log_dir),
        },
        "warnings": warnings,
    }
    output_path = Path(args.output).resolve() if args.output else out_dir / "result.json"
    report["artifacts"]["result_json"] = str(output_path)
    _write_if_changed(output_path, json.dumps(report, indent=2, sort_keys=True) + "\n")
    return report


def _format_rate(value: float) -> str:
    units = [(1e9, "G"), (1e6, "M"), (1e3, "k")]
    for scale, suffix in units:
        if value >= scale:
            return f"{value / scale:.3f} {suffix}/s"
    return f"{value:.3f} /s"


def print_human_report(report: Mapping[str, Any]) -> None:
    operation = str(report["case"]["operation"])
    correctness = report["correctness"]
    print(
        f"correctness preflight: PASS ({correctness['observations']} exact "
        f"{correctness['observation']} port-word observations, "
        f"sha256={correctness['transcript_sha256'][:16]}...)"
    )
    print(f"{'backend':<12} {'median':>12} {operation + '/s':>18} {'p25..p75':>27}")
    for backend in ("cpp", "verilator"):
        stats = report["runs"][backend]["stats"]
        median_ms = float(stats["median_seconds"]) * 1000.0
        median_rate = float(stats["median_throughput_per_second"])
        p25 = _format_rate(float(stats["p25_throughput_per_second"]))
        p75 = _format_rate(float(stats["p75_throughput_per_second"]))
        print(f"{backend:<12} {median_ms:>10.3f} ms {_format_rate(median_rate):>18} {p25 + ' .. ' + p75:>27}")
    comparison = report["comparison"]
    print(f"comparison: {comparison['faster_backend']} is {float(comparison['speedup']):.3f}x faster")
    for warning in report.get("warnings", []):
        print(f"warning: {warning}", file=sys.stderr)
    print(f"result: {report['artifacts']['result_json']}")


def add_benchmark_arguments(parser: argparse.ArgumentParser) -> None:
    parser.add_argument("python_file", help="Python project entry defining build and @testbench tb")
    parser.add_argument("--out-dir", default=None, help="Artifact directory (default: .pycircuit_out/perf/cpp-vs-verilator/<source>)")
    parser.add_argument("--output", default=None, help="Result JSON path (default: <out-dir>/result.json)")
    parser.add_argument("--project-root", default=None, help="Project root passed to the frontend API-contract scan")
    parser.add_argument("--param", action="append", default=[], help="JIT parameter override name=value (repeatable)")
    parser.add_argument("--mode", choices=["auto", "comb", "clocked"], default="auto")
    parser.add_argument("--iterations", type=int, default=1_000_000, help="Timed cycles/evaluations per sample")
    parser.add_argument("--warmup-iterations", type=int, default=10_000, help="Untimed in-process warmup per sample")
    parser.add_argument("--verify-iterations", type=int, default=256, help="Exact preflight cycles/evaluations")
    parser.add_argument("--repeats", type=int, default=5, help="Fresh-process samples per backend")
    parser.add_argument("--seed", type=lambda value: int(value, 0), default=0x6A09E667F3BCC909)
    parser.add_argument("--input-table-size", type=int, default=4096)
    parser.add_argument("--sample-every", type=int, default=256, help="Timed digest interval")
    parser.add_argument("--reset-cycles", type=int, default=2)
    parser.add_argument("--reset-settle-cycles", type=int, default=1)
    parser.add_argument("--logic-depth", type=int, default=256)
    parser.add_argument("--max-port-bits", type=int, default=4096)
    parser.add_argument("--jobs", type=int, default=max(1, os.cpu_count() or 1))
    parser.add_argument("--cpu", type=int, default=None, help="Pin timed child processes to one logical CPU (Linux)")
    parser.add_argument("--native", action="store_true", help="Compile both harnesses with -march=native")
    parser.add_argument("--min-sample-seconds", type=float, default=0.1, help="Warn below this internal timed duration")
    parser.add_argument("--timeout-seconds", type=float, default=300.0, help="Timeout for each build/run stage")
    parser.add_argument("--verilator", default=None, help="Verilator executable")
    parser.add_argument("--cmake", default=None, help="CMake executable")
    parser.add_argument("--cxx", default=os.environ.get("CXX", "c++"), help="C++ compiler used by both backends")


def run_from_namespace(args: argparse.Namespace) -> int:
    try:
        report = run_benchmark(args)
    except BenchmarkError as exc:
        print(f"benchmark error: {exc}", file=sys.stderr)
        return 2
    print_human_report(report)
    return 0


def standalone_main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        prog="pycircuit benchmark",
        description="Compare pyCircuit generated C++ simulation with a trace-free Verilator model.",
    )
    add_benchmark_arguments(parser)
    return run_from_namespace(parser.parse_args(argv))


if __name__ == "__main__":
    raise SystemExit(standalone_main())
