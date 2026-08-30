# Rust 对照编译优化等级提高到最高

日期：2026-08-31  
状态：按用户指示实施  
分支：`feat/rust-emitter`

## 背景与目标

Rust emitter 的功能仿真对照脚本用 `rustc -C opt-level=2` 编 DUT 和 `pyc_runtime`。用户要求把 **Rust 一侧的优化等级提到最高**，再看 microbench 相对 C++ 的 Hz。

**目标**：对照与功能测试里的 `rustc` 使用最高速度优化等级；不改 MLIR 语义，不改 C++ 优化等级。

## 项目现状与执行流程定位

`pycc --emit=rust` 只生成 `.rs`，不调用 `rustc`。真正编译发生在：

1. `tests/rust_emit/runner.py` 的 `build_runtime_rlib`：把 `runtime/rust` 编成 `rlib`。
2. 同文件的 `compile_rust_harness`：把生成 DUT 和 harness 编成可执行文件。
3. `flows/tools/perf/run_rust_vs_cpp.py` 复用上述两个函数，并在 JSON 里记录 `"opt": {"cxx": "-O2", "rustc": "-C opt-level=2"}`。

C++ 对照仍是 `g++ -O2`。`pycc --out-dir` 写出的 `Cargo.toml` 没有自定义 profile；`cargo build --release` 默认已是 `opt-level=3`。

## 相关现有实现说明

`rustc` 的 `-C opt-level` 取值：`0`/`1`/`2`/`3` 为速度档，`s`/`z` 为体积档。数值最高速度档是 **`3`**（等价于 Cargo `--release` 默认）。`lto`、`codegen-units`、`target-cpu` 不是“优化等级”，本次不改。

上一轮 narrow-wire 基线（`opt-level=2`）：microbench C++ 8.82e7 Hz，Rust 1.45e7 Hz。

## 需求与验收标准

1. `build_runtime_rlib` 与 `compile_rust_harness` 使用 `-C opt-level=3`。
2. C++ 仍为 `-O2`。
3. 功能测试仍通过（counter `count=1..5` 与 C++ 一致）。
4. 重跑 `run_rust_vs_cpp.py --repeats 3`，JSON 记录 `rustc: -C opt-level=3`，并把 microbench 中位 Hz 与 1.45e7 对比后写回本文。

## 方案设计

### 模块边界、输入与输出

只改测试/对照用的 `rustc` 命令行。生成代码、runtime 语义、C++ 路径不变。

### 上下游关系与数据/控制流

`pytest` 与性能脚本都走 `runner.py`，改一处两处调用即可。

### 文件和接口改动清单

- `tests/rust_emit/runner.py`：两处 `opt-level=2` → `opt-level=3`。
- `flows/tools/perf/run_rust_vs_cpp.py`：文档字符串与 JSON `opt.rustc`。
- 本文档回写实测。

### 边界条件、错误处理与兼容性

编译更慢一些，功能结果应不变。counter/arith 仍可能被 rustc DCE，microbench 才是可比数字。这是不对称对照（Rust `-O3` vs C++ `-O2`），按用户要求只抬 Rust。

## 与既有文档和约束的一致性检查

| 文档 | 结论 |
|------|------|
| rust-emitter 20260830 / narrow-wires 20260831 | 对照仍用同一脚本；只改 Rust 优化档 |
| pyc4.0 语义在 MLIR | 不改 dialect/pass/C++ |

## 测试与验证计划

```bash
python3 -m pytest tests/rust_emit/test_rust_emitter.py -q -k 'not test_perf'
python3 flows/tools/perf/run_rust_vs_cpp.py --repeats 3
```

基线：narrow-wire + `opt-level=2`，microbench Rust 1.45e7 Hz。

### 实测结果（2026-08-31，`opt-level=3`）

环境同前：`g++ -O2`，`rustc -C opt-level=3`，每项重复 3 次取中位数。JSON：`.pycircuit_out/perf/rust_vs_cpp.json`。

| 设计 | 周期 | C++ Hz（中位） | Rust Hz（中位） | 相对 opt-level=2 Rust |
|------|------|----------------|-----------------|------------------------|
| counter | 2e6 | 1.20e8 | 4.00e9 | 仍 DCE，不可比 |
| arith | 5e6 | 7.98e8 | 3.40e9 | 仍 DCE，不可比 |
| microbench | 2e5 | 8.81e7 | **1.48e7** | 1.45e7 → 1.48e7（约 +2%） |

读数说明：

- 功能：`pytest tests/rust_emit/test_rust_emitter.py -k 'not test_perf'` 4 passed。
- 把 `opt-level` 从 2 提到 3，microbench 几乎不动。相对 C++ 仍约 **6.0×** 慢。
- Rust 编译时间仍约 0.13 s，二进制仍约 4.5 MB。
- 结论：当前瓶颈不在 `opt-level=2` vs `3`，而在生成形态（全量 eval、SSA 写成 struct 字段）。

## 实施步骤

1. 落盘本文。
2. 改 `runner.py` 与性能脚本记录。
3. 功能测试 + 性能对照 + 回写数字。已完成。

## 待确认事项

无。已拍板：Rust 用 `rustc` 最高速度档 `-C opt-level=3`（不是 `s`/`z`，也不加 LTO/`target-cpu`）；C++ 保持 `-O2`。后果：功能仿真对照不再与上一轮同档优化，JSON 会标明档位。
