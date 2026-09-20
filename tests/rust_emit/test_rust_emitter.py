from __future__ import annotations

import json
from pathlib import Path

import pytest

from .runner import (
    REPO_ROOT,
    build_runtime_rlib,
    compile_cpp_harness,
    compile_rust_harness,
    emit_pyc,
    find_pycc,
    find_rustc,
    merged_env,
    pycc_emit,
    run_cmd,
)

HARNESS = Path(__file__).resolve().parent / "harness"
EXAMPLES = REPO_ROOT / "designs" / "examples"


@pytest.fixture(scope="session")
def pycc() -> Path:
    return find_pycc()


def test_emit_rust_contains_sim_api(tmp_path: Path, pycc: Path) -> None:
    pyc = emit_pyc(EXAMPLES / "counter" / "counter.py", tmp_path / "counter.pyc", pycc)
    rust = pycc_emit(pyc, kind="rust", out=tmp_path / "counter.rs", pycc=pycc)
    text = rust.read_text(encoding="utf-8")
    assert "pub struct Counter" in text
    assert "pub fn eval(" in text
    assert "pub fn tick(" in text
    assert "pub fn transfer(" in text
    assert "Wire<" not in text
    assert "clk: bool" in text
    assert "count: u8" in text
    assert "wrapping_add" in text
    out_dir = tmp_path / "rust_out"
    run_cmd(
        [str(pycc), str(pyc), "--emit=rust", "--out-dir", str(out_dir), "--logic-depth=256"],
        env=merged_env(pycc),
    )
    assert (out_dir / "Cargo.toml").is_file()
    assert (out_dir / "lib.rs").is_file()
    manifest = json.loads((out_dir / "manifest.json").read_text(encoding="utf-8"))
    assert manifest.get("rust_modules")


def test_emit_rust_rejects_memory(tmp_path: Path, pycc: Path) -> None:
    src = EXAMPLES / "sync_mem_init_zero" / "sync_mem_init_zero.py"
    pyc = emit_pyc(src, tmp_path / "mem.pyc", pycc)
    with pytest.raises(AssertionError, match="v1 subset|memory/FIFO|sync_mem"):
        pycc_emit(pyc, kind="rust", out=tmp_path / "mem.rs", pycc=pycc)


@pytest.mark.skipif(find_rustc() is None, reason="rustc not found")
def test_counter_rust_and_cpp_functional(tmp_path: Path, pycc: Path) -> None:
    pyc = emit_pyc(EXAMPLES / "counter" / "counter.py", tmp_path / "counter.pyc", pycc)
    rust_src = pycc_emit(pyc, kind="rust", out=tmp_path / "counter.rs", pycc=pycc)
    cpp_src = pycc_emit(pyc, kind="cpp", out=tmp_path / "counter.cpp", pycc=pycc)
    rlib = build_runtime_rlib(tmp_path)
    rust_exe = compile_rust_harness(HARNESS / "counter_main.rs", rust_src, rlib, tmp_path / "counter_rs")
    cpp_exe = compile_cpp_harness(HARNESS / "counter_main.cpp", cpp_src, tmp_path / "counter_cpp")
    rust_out = run_cmd([str(rust_exe)]).stdout
    cpp_out = run_cmd([str(cpp_exe)]).stdout
    assert "ok" in rust_out
    assert "ok" in cpp_out
    rust_counts = [line for line in rust_out.splitlines() if line.startswith("count=")]
    cpp_counts = [line for line in cpp_out.splitlines() if line.startswith("count=")]
    assert rust_counts == cpp_counts == ["count=1", "count=2", "count=3", "count=4", "count=5"]


@pytest.mark.skipif(find_rustc() is None, reason="rustc not found")
def test_arith_rust_and_cpp_functional(tmp_path: Path, pycc: Path) -> None:
    pyc = emit_pyc(EXAMPLES / "arith" / "arith.py", tmp_path / "arith.pyc", pycc)
    rust_src = pycc_emit(pyc, kind="rust", out=tmp_path / "arith.rs", pycc=pycc)
    cpp_src = pycc_emit(pyc, kind="cpp", out=tmp_path / "arith.cpp", pycc=pycc)
    rlib = build_runtime_rlib(tmp_path)
    rust_exe = compile_rust_harness(HARNESS / "arith_main.rs", rust_src, rlib, tmp_path / "arith_rs")
    cpp_exe = compile_cpp_harness(HARNESS / "arith_main.cpp", cpp_src, tmp_path / "arith_cpp")
    assert "ok" in run_cmd([str(rust_exe)]).stdout
    assert "ok" in run_cmd([str(cpp_exe)]).stdout


@pytest.mark.skipif(find_rustc() is None, reason="rustc not found")
def test_perf_script_writes_json(tmp_path: Path, pycc: Path) -> None:
    out_json = tmp_path / "rust_vs_cpp.json"
    run_cmd(
        [
            "python3",
            str(REPO_ROOT / "flows" / "tools" / "perf" / "run_rust_vs_cpp.py"),
            "--out",
            str(out_json),
            "--repeats",
            "1",
            "--cycles-counter",
            "20000",
            "--cycles-arith",
            "50000",
            "--cycles-microbench",
            "2000",
        ],
        cwd=REPO_ROOT,
    )
    data = json.loads(out_json.read_text(encoding="utf-8"))
    assert data["designs"]
    for row in data["designs"]:
        assert row["cpp"]["hz"] > 0
        assert row["rust"]["hz"] > 0
