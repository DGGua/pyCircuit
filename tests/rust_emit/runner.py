from __future__ import annotations

import json
import os
import shutil
import subprocess
import sys
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[2]


def pythonpath() -> str:
    parts = [str(REPO_ROOT / "compiler" / "frontend"), str(REPO_ROOT / "designs")]
    old = os.environ.get("PYTHONPATH")
    if old:
        parts.append(old)
    return ":".join(parts)


def find_pycc() -> Path:
    env = os.environ.get("PYCC")
    if env:
        p = Path(env)
        if p.is_file() and os.access(p, os.X_OK):
            return p
        raise FileNotFoundError(f"PYCC is set but not executable: {p}")
    toolchain = os.environ.get("PYC_TOOLCHAIN_ROOT")
    candidates = [
        Path(toolchain) / "bin" / "pycc" if toolchain else None,
        REPO_ROOT / ".pycircuit_out" / "toolchain" / "install" / "bin" / "pycc",
        REPO_ROOT / ".pycircuit_out" / "toolchain" / "build" / "bin" / "pycc",
    ]
    for c in candidates:
        if c is not None and c.is_file() and os.access(c, os.X_OK):
            return c
    found = shutil.which("pycc")
    if found:
        return Path(found)
    raise FileNotFoundError("missing pycc (set PYCC or PYC_TOOLCHAIN_ROOT)")


def find_rustc() -> Path | None:
    found = shutil.which("rustc")
    return Path(found) if found else None


def run_cmd(cmd: list[str], *, cwd: Path | None = None, env: dict[str, str] | None = None) -> subprocess.CompletedProcess[str]:
    proc = subprocess.run(
        cmd,
        cwd=str(cwd or REPO_ROOT),
        env=env,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        check=False,
    )
    if proc.returncode != 0:
        raise AssertionError(
            f"command failed ({proc.returncode}): {' '.join(cmd)}\n"
            f"stdout:\n{proc.stdout}\nstderr:\n{proc.stderr}"
        )
    return proc


def merged_env(pycc: Path) -> dict[str, str]:
    env = os.environ.copy()
    env["PYTHONPATH"] = pythonpath()
    env["PYCC"] = str(pycc)
    if pycc.parent.name == "bin":
        env.setdefault("PYC_TOOLCHAIN_ROOT", str(pycc.parent.parent))
    env.setdefault("PYC_RUST_RUNTIME_DIR", str(REPO_ROOT / "runtime" / "rust"))
    return env


def emit_pyc(src: Path, out_pyc: Path, pycc: Path) -> Path:
    out_pyc.parent.mkdir(parents=True, exist_ok=True)
    run_cmd(
        [sys.executable, "-m", "pycircuit.cli", "emit", str(src), "-o", str(out_pyc)],
        env=merged_env(pycc),
    )
    if not out_pyc.is_file():
        raise AssertionError(f"frontend did not write {out_pyc}")
    return out_pyc


def pycc_emit(pyc: Path, *, kind: str, out: Path, pycc: Path, logic_depth: int = 256) -> Path:
    out.parent.mkdir(parents=True, exist_ok=True)
    run_cmd(
        [str(pycc), str(pyc), f"--emit={kind}", "-o", str(out), f"--logic-depth={logic_depth}"],
        env=merged_env(pycc),
    )
    return out


def build_runtime_rlib(out_dir: Path) -> Path:
    rustc = find_rustc()
    if rustc is None:
        raise FileNotFoundError("rustc not found")
    rlib = out_dir / "libpyc_runtime.rlib"
    out_dir.mkdir(parents=True, exist_ok=True)
    run_cmd(
        [
            str(rustc),
            "--edition",
            "2021",
            "--crate-type",
            "rlib",
            "--crate-name",
            "pyc_runtime",
            "-C",
            "opt-level=3",
            str(REPO_ROOT / "runtime" / "rust" / "src" / "lib.rs"),
            "-o",
            str(rlib),
        ]
    )
    return rlib


def compile_rust_harness(harness_rs: Path, dut_rs: Path, rlib: Path, exe: Path) -> Path:
    rustc = find_rustc()
    if rustc is None:
        raise FileNotFoundError("rustc not found")
    wrapper = exe.parent / f"{exe.name}_wrapper.rs"
    wrapper.write_text(
        "#![allow(non_camel_case_types, non_snake_case, unused_imports, dead_code, unused_parens)]\n"
        f"include!({json.dumps(str(dut_rs))});\n"
        f"include!({json.dumps(str(harness_rs))});\n",
        encoding="utf-8",
    )
    run_cmd(
        [
            str(rustc),
            "--edition",
            "2021",
            "--crate-name",
            exe.name.replace("-", "_"),
            "-C",
            "opt-level=3",
            "--extern",
            f"pyc_runtime={rlib}",
            str(wrapper),
            "-o",
            str(exe),
        ]
    )
    return exe


def compile_cpp_harness(harness_cpp: Path, dut_cpp: Path, exe: Path) -> Path:
    cmd = [
        "g++",
        "-std=c++17",
        "-O3",
        f"-I{REPO_ROOT / 'runtime'}",
        f"-include",
        str(dut_cpp),
        str(harness_cpp),
        "-o",
        str(exe),
    ]
    run_cmd(cmd)
    return exe
