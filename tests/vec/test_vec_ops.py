from __future__ import annotations

from pathlib import Path

import pytest

from .cases import FRONTEND_ONLY_CASES, FULL_BACKEND_CASES, VEC_CASES, VecCase
from .generate import render_case_source
from .runner import assert_verilator_ran, check_cpp_manifest_syntax, check_ir, merged_env, run_cmd, run_cpp_binary, run_vec_case


def _run_yosys_smoke(verilog: Path, *, top: str, repo_root: Path) -> None:
    import shutil

    yosys = shutil.which("yosys")
    if yosys is None:
        pytest.skip("yosys not found")
    script = verilog.with_suffix(".ys")
    script.write_text(
        "\n".join(
            [
                f"read_verilog -I{repo_root / 'runtime' / 'verilog'} {verilog}",
                f"hierarchy -top {top}",
                "proc",
                "opt",
                "stat",
            ]
        )
        + "\n",
        encoding="utf-8",
    )
    run_cmd([yosys, "-q", "-s", str(script)], cwd=repo_root)


@pytest.mark.vec
@pytest.mark.slow
@pytest.mark.parametrize("case", FULL_BACKEND_CASES, ids=[case.name for case in FULL_BACKEND_CASES])
def test_vec_operator_case(
    case: VecCase,
    *,
    repo_root: Path,
    vec_test_root: Path,
    pyc_pythonpath: str,
    pycc: Path,
    verilator: str | None,
) -> None:
    run_vec_case(
        case,
        repo_root=repo_root,
        out_root=vec_test_root,
        pythonpath=pyc_pythonpath,
        pycc=pycc,
        verilator=verilator,
    )


@pytest.mark.vec
def test_case_matrix_has_minimum_coverage() -> None:
    kinds = {case.kind for case in VEC_CASES}
    required = {
        "add_vv",
        "add_vs",
        "add_sv",
        "eq_vv",
        "eq_vs",
        "eq_sv",
        "sub_vs",
        "sub_sv",
        "or_reduce",
        "reduce_sum",
        "reduce_sum_signed",
        "select_vv",
        "select_vs",
        "select_sv",
        "zext",
        "sext",
        "slice",
    }
    assert required <= kinds


@pytest.mark.vec
def test_true_division_is_rejected(repo_root: Path) -> None:
    import sys

    frontend = repo_root / "compiler" / "frontend"
    if str(frontend) not in sys.path:
        sys.path.insert(0, str(frontend))

    from pycircuit import Circuit

    m = Circuit("vec_true_division_rejected")
    a = m.vec([m.input(f"a{i}", width=4) for i in range(4)])
    b = m.vec([m.input(f"b{i}", width=4) for i in range(4)])

    with pytest.raises(TypeError, match="use `//`"):
        _ = a / b
    with pytest.raises(TypeError, match="use `//`"):
        _ = a / 3
    with pytest.raises(TypeError, match="use `//`"):
        _ = 12 / a
    with pytest.raises(TypeError, match="use `//`"):
        _ = a[0] / b[0]
        compile(build, name="vec_true_division_jit_rejected")


@pytest.mark.vec
@pytest.mark.parametrize("case", FRONTEND_ONLY_CASES, ids=[case.name for case in FRONTEND_ONLY_CASES])
def test_frontend_only_case_generation(case: VecCase) -> None:
    source = render_case_source(case)
    assert "m.vec([" in source
    assert case.kind in source or case.name in source


@pytest.mark.vec
def test_vector_io_emit_and_pycc(
    *,
    repo_root: Path,
    vec_test_root: Path,
    pyc_pythonpath: str,
    pycc: Path,
) -> None:
    case_root = vec_test_root / "vector_io"
    src_dir = case_root / "src"
    out_dir = case_root / "build"
    src_dir.mkdir(parents=True, exist_ok=True)
    out_dir.mkdir(parents=True, exist_ok=True)
    src = src_dir / "vector_io.py"
    src.write_text(
        "\n".join(
            [
                "from __future__ import annotations",
                "",
                "from pycircuit import Circuit, module",
                "",
                "",
                "@module",
                "def build(m: Circuit) -> None:",
                '    a = m.input("a", width=4, shape=4)',
                "    out = a + a",
                '    m.output("out", out)',
                "",
            ]
        ),
        encoding="utf-8",
    )
    env = merged_env(pythonpath=pyc_pythonpath, pycc=pycc)
    pyc = out_dir / "vector_io.pyc"
    run_cmd(["python3", "-m", "pycircuit.cli", "emit", str(src), "-o", str(pyc)], cwd=repo_root, env=env)
    mlir = pyc.read_text(encoding="utf-8")
    check_ir(VecCase("vector_io", "vector_io", ir_tokens=("vector<", "pyc.add")), mlir)
    cpp_dir = out_dir / "cpp"
    run_cmd(
        [str(pycc), str(pyc), "--emit=cpp", "--cpp-split=module", "--out-dir", str(cpp_dir), "--build-profile=dev-fast"],
        cwd=repo_root,
        env=env,
    )
    verilog = out_dir / "vector_io.v"
    run_cmd([str(pycc), str(pyc), "--emit=verilog", "-o", str(verilog), "--build-profile=dev-fast"], cwd=repo_root, env=env)
    verilog_text = verilog.read_text(encoding="utf-8")
    assert "input [15:0] a" in verilog_text
    assert "output [15:0] out" in verilog_text
    assert "input [3:0] a [0:3]" not in verilog_text
    assert "output [3:0] out [0:3]" not in verilog_text
    check_cpp_manifest_syntax(out_dir, repo_root=repo_root)


@pytest.mark.vec
def test_rank2_vector_io_verilog_uses_packed_ports_and_yosys(
    *,
    repo_root: Path,
    vec_test_root: Path,
    pyc_pythonpath: str,
    pycc: Path,
) -> None:
    case_root = vec_test_root / "rank2_vector_io"
    src_dir = case_root / "src"
    out_dir = case_root / "build"
    src_dir.mkdir(parents=True, exist_ok=True)
    out_dir.mkdir(parents=True, exist_ok=True)
    src = src_dir / "rank2_vector_io.py"
    src.write_text(
        "\n".join(
            [
                "from __future__ import annotations",
                "",
                "from pycircuit import Circuit, module",
                "",
                "",
                "@module",
                "def build(m: Circuit) -> None:",
                '    a = m.input("a", width=2, shape=(2, 3))',
                "    out = a + a",
                '    m.output("out", out)',
                "",
            ]
        ),
        encoding="utf-8",
    )
    env = merged_env(pythonpath=pyc_pythonpath, pycc=pycc)
    pyc = out_dir / "rank2_vector_io.pyc"
    run_cmd(["python3", "-m", "pycircuit.cli", "emit", str(src), "-o", str(pyc)], cwd=repo_root, env=env)
    verilog = out_dir / "rank2_vector_io.v"
    run_cmd([str(pycc), str(pyc), "--emit=verilog", "-o", str(verilog), "--build-profile=dev-fast"], cwd=repo_root, env=env)
    verilog_text = verilog.read_text(encoding="utf-8")
    assert "input [11:0] a" in verilog_text
    assert "output [11:0] out" in verilog_text
    assert "input [1:0] a [0:1][0:2]" not in verilog_text
    assert "output [1:0] out [0:1][0:2]" not in verilog_text
    _run_yosys_smoke(verilog, top="Rank2VectorIo", repo_root=repo_root)


@pytest.mark.vec
def test_vector_port_cli_build_runs_tb(
    *,
    repo_root: Path,
    vec_test_root: Path,
    pyc_pythonpath: str,
    pycc: Path,
    verilator: str | None,
) -> None:
    case_root = vec_test_root / "vector_port_tb"
    src_dir = case_root / "src"
    out_dir = case_root / "build"
    src_dir.mkdir(parents=True, exist_ok=True)
    out_dir.mkdir(parents=True, exist_ok=True)
    src = src_dir / "vector_port_tb.py"
    src.write_text(
        "\n".join(
            [
                "from __future__ import annotations",
                "",
                "from pycircuit import Circuit, Tb, module, testbench",
                "",
                "",
                "@module",
                "def build(m: Circuit) -> None:",
                '    a = m.input("a", width=4, shape=4)',
                "    out = a + 1",
                '    m.output("out", out)',
                "",
                "",
                "@testbench",
                "def tb(t: Tb) -> None:",
                "    t.timeout(1)",
                '    t.drive("a", 0x4321, at=0)',
                '    t.expect("out", 0x5432, at=0, msg="vector out")',
                "    t.finish(at=0)",
                "",
            ]
        ),
        encoding="utf-8",
    )
    env = merged_env(pythonpath=pyc_pythonpath, pycc=pycc)
    run_cmd(
        [
            "python3",
            "-m",
            "pycircuit.cli",
            "build",
            str(src),
            "--out-dir",
            str(out_dir),
            "--target",
            "cpp",
            "--jobs",
            "2",
            "--logic-depth",
            "64",
            "--profile",
            "dev",
        ],
        cwd=repo_root,
        env=env,
    )
    run_cpp_binary(out_dir)
    if verilator:
        verilator_out = case_root / "build_verilator"
        run_cmd(
            [
                "python3",
                "-m",
                "pycircuit.cli",
                "build",
                str(src),
                "--out-dir",
                str(verilator_out),
                "--target",
                "both",
                "--jobs",
                "2",
                "--logic-depth",
                "64",
                "--profile",
                "dev",
                "--run-verilator",
            ],
            cwd=repo_root,
            env=env,
        )
        assert_verilator_ran(verilator_out)


@pytest.mark.vec
def test_eager_vec_broadcast_emit_and_pycc(
    *,
    repo_root: Path,
    vec_test_root: Path,
    pyc_pythonpath: str,
    pycc: Path,
) -> None:
    case_root = vec_test_root / "eager_broadcast"
    src_dir = case_root / "src"
    out_dir = case_root / "build"
    src_dir.mkdir(parents=True, exist_ok=True)
    out_dir.mkdir(parents=True, exist_ok=True)
    src = src_dir / "eager_broadcast.py"
    src.write_text(
        "\n".join(
            [
                "from __future__ import annotations",
                "",
                "from pycircuit import Circuit, module",
                "",
                "",
                "@module",
                "def build(m: Circuit) -> None:",
                '    a = m.vec([m.input(f"a{i}", width=4) for i in range(4)])',
                "    out = a.broadcast(dim=1, size=2)",
                '    m.output("out", out)',
                "",
            ]
        ),
        encoding="utf-8",
    )
    env = merged_env(pythonpath=pyc_pythonpath, pycc=pycc)
    pyc = out_dir / "eager_broadcast.pyc"
    run_cmd(["python3", "-m", "pycircuit.cli", "emit", str(src), "-o", str(pyc)], cwd=repo_root, env=env)
    mlir = pyc.read_text(encoding="utf-8")
    check_ir(VecCase("eager_broadcast", "eager_broadcast", ir_tokens=("vector<", "pyc.v_create", "pyc.v_broadcast_dim")), mlir)
    cpp_dir = out_dir / "cpp"
    run_cmd(
        [str(pycc), str(pyc), "--emit=cpp", "--cpp-split=module", "--out-dir", str(cpp_dir), "--build-profile=dev-fast"],
        cwd=repo_root,
        env=env,
    )
    run_cmd([str(pycc), str(pyc), "--emit=verilog", "-o", str(out_dir / "eager_broadcast.v"), "--build-profile=dev-fast"], cwd=repo_root, env=env)
    check_cpp_manifest_syntax(out_dir, repo_root=repo_root)


@pytest.mark.vec
def test_jit_instance_vec_port_emit_and_pycc(
    *,
    repo_root: Path,
    vec_test_root: Path,
    pyc_pythonpath: str,
    pycc: Path,
) -> None:
    case_root = vec_test_root / "jit_instance_vec_port"
    src_dir = case_root / "src"
    out_dir = case_root / "build"
    src_dir.mkdir(parents=True, exist_ok=True)
    out_dir.mkdir(parents=True, exist_ok=True)
    src = src_dir / "jit_instance_vec_port.py"
    src.write_text(
        "\n".join(
            [
                "from __future__ import annotations",
                "",
                "from pycircuit import Circuit, module",
                "",
                "",
                "@module",
                "def child(m: Circuit, a):",
                "    return a + a",
                "",
                "",
                "@module",
                "def build(m: Circuit) -> None:",
                '    a = m.vec([m.input(f"a{i}", width=4) for i in range(4)])',
                '    y = m.instance(child, name="u_child", a=a).read()',
                '    m.output("out", y)',
                "",
            ]
        ),
        encoding="utf-8",
    )
    env = merged_env(pythonpath=pyc_pythonpath, pycc=pycc)
    pyc = out_dir / "jit_instance_vec_port.pyc"
    run_cmd(["python3", "-m", "pycircuit.cli", "emit", str(src), "-o", str(pyc)], cwd=repo_root, env=env)
    mlir = pyc.read_text(encoding="utf-8")
    check_ir(
        VecCase("jit_instance_vec_port", "jit_instance_vec_port", ir_tokens=("pyc.instance", "vector<4xi4>", "pyc.v_create")),
        mlir,
    )
    cpp_dir = out_dir / "cpp"
    run_cmd(
        [str(pycc), str(pyc), "--emit=cpp", "--cpp-split=module", "--out-dir", str(cpp_dir), "--build-profile=dev-fast"],
        cwd=repo_root,
        env=env,
    )
    verilog = out_dir / "jit_instance_vec_port.v"
    run_cmd([str(pycc), str(pyc), "--emit=verilog", "-o", str(verilog), "--build-profile=dev-fast"], cwd=repo_root, env=env)
    verilog_text = verilog.read_text(encoding="utf-8")
    assert "input [15:0] a" in verilog_text
    assert "__flat" in verilog_text
    assert "input [3:0] a [0:3]" not in verilog_text
    _run_yosys_smoke(verilog, top="JitInstanceVecPort", repo_root=repo_root)
    check_cpp_manifest_syntax(out_dir, repo_root=repo_root)


@pytest.mark.vec
def test_dim_reduce_emit_and_pycc(
    *,
    repo_root: Path,
    vec_test_root: Path,
    pyc_pythonpath: str,
    pycc: Path,
) -> None:
    case_root = vec_test_root / "dim_reduce"
    src_dir = case_root / "src"
    out_dir = case_root / "build"
    src_dir.mkdir(parents=True, exist_ok=True)
    out_dir.mkdir(parents=True, exist_ok=True)
    src = src_dir / "dim_reduce.py"
    src.write_text(
        "\n".join(
            [
                "from __future__ import annotations",
                "",
                "from pycircuit import Circuit, module",
                "",
                "",
                "@module",
                "def build(m: Circuit) -> None:",
                '    a = m.input("a", width=1, shape=(2, 3))',
                '    m.output("or0", a.or_reduce(dim=0))',
                '    m.output("or1", a.or_reduce(dim=1))',
                '    m.output("and0", a.and_reduce(dim=0))',
                '    m.output("and1", a.and_reduce(dim=1))',
                '    m.output("sum0", a.reduce_sum(dim=0))',
                '    m.output("sum1", a.reduce_sum(dim=1))',
                "",
            ]
        ),
        encoding="utf-8",
    )
    env = merged_env(pythonpath=pyc_pythonpath, pycc=pycc)
    pyc = out_dir / "dim_reduce.pyc"
    run_cmd(["python3", "-m", "pycircuit.cli", "emit", str(src), "-o", str(pyc)], cwd=repo_root, env=env)
    check_ir(
        VecCase("dim_reduce", "dim_reduce", ir_tokens=("vector<", "pyc.v_or_reduce", "pyc.v_and_reduce", "pyc.v_add_reduce")),
        pyc.read_text(encoding="utf-8"),
    )
    cpp_dir = out_dir / "cpp"
    run_cmd(
        [str(pycc), str(pyc), "--emit=cpp", "--cpp-split=module", "--out-dir", str(cpp_dir), "--build-profile=dev-fast"],
        cwd=repo_root,
        env=env,
    )
    run_cmd([str(pycc), str(pyc), "--emit=verilog", "-o", str(out_dir / "dim_reduce.v"), "--build-profile=dev-fast"], cwd=repo_root, env=env)
    check_cpp_manifest_syntax(out_dir, repo_root=repo_root)


@pytest.mark.vec
def test_reduce_mode_attr_and_pycc(
    *,
    repo_root: Path,
    vec_test_root: Path,
    pyc_pythonpath: str,
    pycc: Path,
) -> None:
    import sys

    frontend = repo_root / "compiler" / "frontend"
    if str(frontend) not in sys.path:
        sys.path.insert(0, str(frontend))

    from pycircuit import Circuit

    m = Circuit("reduce_mode_invalid")
    bad = m.vec([m.input(f"bad{i}", width=1) for i in range(2)])
    with pytest.raises(ValueError, match="reduce mode"):
        bad.or_reduce(mode="flat")

    case_root = vec_test_root / "reduce_modes"
    src_dir = case_root / "src"
    out_dir = case_root / "build"
    src_dir.mkdir(parents=True, exist_ok=True)
    out_dir.mkdir(parents=True, exist_ok=True)
    src = src_dir / "reduce_modes.py"
    src.write_text(
        "\n".join(
            [
                "from __future__ import annotations",
                "",
                "from pycircuit import Circuit, module",
                "",
                "",
                "@module",
                "def build(m: Circuit) -> None:",
                '    a = m.input("a", width=1, shape=4)',
                '    b = m.input("b", width=2, shape=4)',
                '    m.output("chain_or", a.or_reduce())',
                '    m.output("tree_or", a.or_reduce(mode="tree"))',
                '    m.output("tree_sum", b.reduce_sum(width=4, mode="tree"))',
                "",
            ]
        ),
        encoding="utf-8",
    )
    env = merged_env(pythonpath=pyc_pythonpath, pycc=pycc)
    pyc = out_dir / "reduce_modes.pyc"
    run_cmd(["python3", "-m", "pycircuit.cli", "emit", str(src), "-o", str(pyc)], cwd=repo_root, env=env)
    mlir = pyc.read_text(encoding="utf-8")
    assert "pyc.v_or_reduce" in mlir
    assert "pyc.v_add_reduce" in mlir
    assert mlir.count('mode = "tree"') == 2
    assert 'mode = "chain"' not in mlir

    direct_verilog = out_dir / "reduce_modes.v"
    run_cmd([str(pycc), str(pyc), "--emit=verilog", "-o", str(direct_verilog), "--build-profile=dev-fast"], cwd=repo_root, env=env)
    direct_text = direct_verilog.read_text(encoding="utf-8")
    assert "chain_or" in direct_text
    assert "tree_or" in direct_text
    assert "(((a__vec[0] | a__vec[1]) | a__vec[2]) | a__vec[3])" in direct_text
    assert "((a__vec[0] | a__vec[1]) | (a__vec[2] | a__vec[3]))" in direct_text

    unrolled_verilog = out_dir / "reduce_modes_unroll.v"
    run_cmd(
        [str(pycc), str(pyc), "--emit=verilog", "-o", str(unrolled_verilog), "--build-profile=dev-fast", "--unroll-vector"],
        cwd=repo_root,
        env=env,
    )
    unrolled_text = unrolled_verilog.read_text(encoding="utf-8")
    assert "v_or_reduce" not in unrolled_text
    assert "v_add_reduce" not in unrolled_text
    assert "assign pyc_or_7 = (pyc_or_6 | pyc_v_get_4);" in unrolled_text
    assert "assign pyc_or_9 = (pyc_or_5 | pyc_or_8);" in unrolled_text
