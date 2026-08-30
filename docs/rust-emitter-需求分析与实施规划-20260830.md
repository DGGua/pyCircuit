# Rust emitter 首版（子集）需求分析与实施规划

日期：2026-08-30  
状态：已按批准方案实施（v1 子集）  
分支：`feat/rust-emitter`

## 背景与目标

pyCircuit 把 Python 电路描述编译成 MLIR，再由 `pycc` 发射后端代码。当前只有两个 emitter：

- C++：功能仿真主路径，生成每个模块一个 `struct`，依赖 `runtime/cpp/` 的 `Wire` / `pyc_reg` 等。
- Verilog：综合与 Verilator 对照。

用户要求：新增一个输出 Rust 的 emitter，并和 C++ 做性能比较。pyc4.0 要求语义留在 dialect 与 MLIR pass，新 emitter 只能消费同一份过 legality gate 的 IR。

**已拍板的首版范围**：功能仿真子集（标量 `iN` + 常见组合运算 + `pyc.reg` + `pyc.instance` + `eval`/`tick`/`transfer`）。先跑通 `counter` 等小程序；大型测试后续再加。不设「Rust 必须更快」门槛。

## 项目现状与执行流程定位

```text
Python 设计（如 designs/examples/counter/counter.py）
  → pycircuit 前端 emit .pyc（MLIR 文本）
  → pycc 跑统一 pass 管线（契约 / 降低 / no-dynamic / flat-types / comb-cycle …）
  → 过 gate 后调用 emitter
       → CppEmitter   → runtime/cpp  → g++ 编译 → 仿真
       → VerilogEmitter → runtime/verilog → 综合/Verilator
       → RustEmitter（新增）→ runtime/rust → rustc 编译 → 仿真
```

`pycc` 入口在 `compiler/mlir/tools/pycc.cpp`。`--emit` 现为 `verilog|cpp|none`。C++ 发射器约 3000 行，还包含 placement 分片、实例输入缓存、SCC/fixpoint、Probe/VCD 等，首版 Rust 不复刻这些。

仿真一步（与 C++ testbench 一致）：

```text
eval → clk=1 → tick → transfer → eval → clk=0 → tick → transfer
```

`tick` = `tick_compute`（上升沿算 next），`transfer` = `tick_commit`（提交 q）。

## 相关现有实现说明

- `compiler/mlir/lib/Emit/CppEmitter.cpp`：每个 `func.func` 生成 `namespace pyc::gen { struct Name { ... } }`，含端口、内部 wire、子模块、`eval`/`tick_compute`/`tick_commit`。缺 `pyc-cpp-placement` 摘要会失败。
- `compiler/mlir/lib/Emit/VerilogEmitter.cpp`：Verilog netlist；接口是 `emitVerilog` / `emitVerilogFunc`。
- `runtime/cpp/pyc_bits.hpp`：`Wire<N>` / `Bits<N>`，64-bit word 数组，宽度掩码。
- `runtime/cpp/pyc_primitives.hpp`：`pyc_reg` 两阶段更新（posedge 时 reset→init，否则 enable→d）。
- `runtime/cpp/pyc_tb.hpp`：完整 testbench（时钟、复位、VCD）。首版 Rust 不接这条路径。
- `designs/examples/counter/`：8-bit 计数器，隐式 `clk`/`rst`，`enable` 控制 `count+1`。`tb_counter.py` 复位 2 拍后期望 count=1..5。
- `designs/examples/arith/`：纯组合加减与常量输出。
- `docs/rfcs/pyc4.0-decisions.md`：Decision 0001/0011（每模块一类 SimObject，`tick`/`transfer`）、0012（父拥有子）、0014（runtime 默认无第三方依赖）、0112（G2 仍是 C++↔Verilog）。

`hier_modules.py` 是 Python 函数内联，不是 `pyc.instance`；instance 仍要实现，以便层次设计与 Decision 0011/0012 对齐。

## 需求与验收标准

1. `pycc --emit=rust` 与 `--rust <path>` 能从过 gate 的 `.pyc` 生成 Rust。
2. `--out-dir` + `--emit=rust` 按模块写 `.rs`，并给出可编译的 `Cargo.toml`（依赖 `runtime/rust`）。
3. 生成代码对每个模块提供 `eval` / `tick` / `transfer`（及 `tick_compute` / `tick_commit`）。
4. 首版支持：标量 `iN`（宽度 1..=64）、算术/位运算/比较/mux/移位/截取/拼接、`pyc.constant` / `pyc.alias` / `pyc.reset_active` / `pyc.assign` / `pyc.comb` / `pyc.reg` / `pyc.instance`。
5. 向量、宽度 >64、mem/FIFO/CDC 等必须 `emitError`，诊断说明是首版子集，不静默跳过。
6. `counter` 用独立 harness 能复位并数到 1..5（对齐 `tb_counter` 激励，不发射 sidecar testbench）。
7. 同一 `.pyc`、同一激励下 C++ 与 Rust 端口轨迹一致。
8. `flows/tools/perf/run_rust_vs_cpp.py` 产出 `.pycircuit_out/perf/rust_vs_cpp.json`（编译时间、二进制大小、Hz）。无「谁必须更快」门槛。
9. 无 `rustc` 时：emit 单测仍过；编译/运行/性能 skip 并说明原因。
10. 不改 MLIR 语义、不改 C++/Verilog emitter 行为、不改 `pycircuit.cli build --target`。

## 方案设计

### 模块边界、输入与输出

**Rust runtime（`runtime/rust/`）**

- 输入：宽度 1..=64 的位向量与寄存器端口引用。
- 输出：`Wire<W>` 运算（截断到 W）与 `PycReg` 两阶段更新。
- 只依赖 Rust std（对齐 Decision 0014）。
- 子模块用 `Box<Child>`（对齐 Decision 0012）。

**RustEmitter**

- 输入：过 gate 的 `ModuleOp`（与 C++ 同一管线）。
- 输出：Rust 源。忽略 `pyc.cpp.*` placement 属性；全部 SSA 值放 struct 字段。
- 不依赖 `pyc-cpp-placement` 摘要。
- 不做实例 eval 缓存、SCC/fixpoint、文件拆分。

**pycc**

- `--emit` 增加 `rust`；新增 `--rust <path>`。
- 单文件 `-o`：全部模块写一个文件。
- `--out-dir`：每模块一个 `.rs` + 最小 `Cargo.toml` + manifest 条目。

**harness / 性能脚本**

- 手写 C++/Rust `main`，同一 `step()` 协议，关闭 VCD/probe/stats。
- 用例：`counter`、`arith`、组合密集 microbench。

### 上下游关系与数据/控制流

前端与 pass 管线不变。Rust 只在 emit 阶段分叉。功能对照与性能对照都从同一 `.pyc` 出发，分别发射 C++ 与 Rust，再用同一激励跑。

### 文件和接口改动清单

新增：

- `compiler/mlir/include/pyc/Emit/RustEmitter.h`：`emitRust` / `emitRustFunc`
- `compiler/mlir/lib/Emit/RustEmitter.cpp`
- `runtime/rust/Cargo.toml` 与 `src/{lib,bits,reg}.rs`
- `tests/rust_emit/`（pytest + harness）
- `flows/tools/perf/run_rust_vs_cpp.py`
- 本文档

修改：

- `compiler/mlir/CMakeLists.txt`：pycc 链上 `RustEmitter.cpp`
- `compiler/mlir/tools/pycc.cpp`：`--emit=rust` / `--rust` / out-dir
- `docs/PIPELINE.md`、`docs/v6_PyCircuit_Software_Architecture.md`

不改：dialect、Transforms、CppEmitter/VerilogEmitter 语义、`pycircuit.cli build --target`、Linx/XiangShan。

### 边界条件、错误处理与兼容性

- 宽度 0 或 >64、向量类型：emitter 报错。
- 不支持的 op：`emitError`，文案标明 rust v1 subset。
- 实例图有环：与 C++ 一样在拓扑排序时失败。
- 单块 `func` 以外：报错（与 C++ 当前契约一致）。
- 默认 `build --target` 不变，避免未完成的 testbench/CMake 进入主流程。

## 与既有文档和约束的一致性检查

| 文档/约束 | 检查结论 |
|-----------|----------|
| `docs/rfcs/pyc4.0-decisions.md` 0001/0011/0012/0014 | Rust 镜像 SimObject/`tick`/`transfer`/父子所有权/无第三方依赖；不改 C++ 契约 |
| Decision 0112 G2 | 仍是 C++↔Verilog；本次只加子集 C++↔Rust 对照 |
| `docs/updatePLAN.md` gate-first / 无 backend-only 语义 | 不新增语义 pass；Rust 只读过 gate 的 IR |
| `docs/PIPELINE.md` / 架构文档 | 实施后补充实验性 `--emit=rust` 与子集边界 |
| `docs/simulation.md` C++ 优化（缓存/SCC/PGO/`-Os`） | v1 对比「朴素全量 eval」；文档写明生产 C++ 还有额外优化 |
| `$pyc4` / `$pyc-build-v40` skill | 本 checkout 不存在；构建仍用 `PYC_TOOLCHAIN_ROOT=.pycircuit_out/toolchain/install` |

无需要用户再拍板的冲突。Rust 不写入 pyc4.0 主里程碑，除非另开 decision。

## 测试与验证计划

1. **emit**：`pycc --emit=rust` 产物含 `struct`、`fn eval`、`fn tick`、`fn transfer`。
2. **负例**：向量或 mem 设计必须失败，诊断可读。
3. **功能**：counter harness 退出码 0（复位后 count=1..5）。
4. **等价**：同一激励下 C++ 与 Rust 输出一致。
5. **性能**：`python3 flows/tools/perf/run_rust_vs_cpp.py`，重复 ≥3 次取中位数，写 JSON。编译：`g++ -O2` 与 `rustc --edition 2021 -C opt-level=2`。
6. **回归**：现有 G2 不改；至少保证 pycc 能编过、新测通过。

可复现命令（实施后）：

```bash
export PYC_TOOLCHAIN_ROOT="$PWD/.pycircuit_out/toolchain/install"
export PYTHONPATH="$PWD/compiler/frontend${PYTHONPATH:+:$PYTHONPATH}"
python3 -m pytest tests/rust_emit/test_rust_emitter.py -q
python3 flows/tools/perf/run_rust_vs_cpp.py
```

### 实测结果（2026-08-30）

环境：本机 Linux，`g++ -O2`，`rustc --edition 2021 -C opt-level=2`，每项重复 3 次取中位数。脚本：`flows/tools/perf/run_rust_vs_cpp.py`。完整 JSON：`.pycircuit_out/perf/rust_vs_cpp.json`。

| 设计 | 周期 | C++ Hz（中位） | Rust Hz（中位） | C++ 编译 s | Rust 编译 s | C++ 二进制 | Rust 二进制 |
|------|------|----------------|-----------------|------------|-------------|------------|-------------|
| counter | 2e6 | 1.08e8 | 1.99e9 | 0.38 | 0.12 | 20 KB | 4.5 MB |
| arith | 5e6 | 7.99e8 | 1.99e9 | 0.37 | 0.11 | 19 KB | 4.5 MB |
| microbench（32 级 add/mux/reg） | 2e5 | 8.79e7 | 1.24e7 | 0.40 | 0.13 | 19 KB | 4.5 MB |

读数说明：

- 这是「朴素全量 eval」对照，不是生产 C++（缓存/SCC/PGO/`-Os`）对照。
- counter / arith 极小，`rustc` 能把循环折掉很多，Hz 偏高，不宜当 ISA 对比。
- microbench 更有信息量：同一激励下 C++ 约 88 MHz，Rust 约 12 MHz。
- Rust 二进制大约 4.5 MB，主要来自链接 `std`；C++ 只链进很小的生成 DUT。
- 功能：counter 两边 `count=1..5` 一致；mem 设计 Rust emit 按子集报错。
- pytest：`tests/rust_emit/test_rust_emitter.py` 本地通过。

后续探索（2026-08-31）：DUT 字段改为 `bool`/`u8`/`u16`/`u32`/`u64`，见 `docs/rust-emitter-narrow-wires-需求分析与实施规划-20260831.md`。microbench Rust 中位从 1.24e7 Hz 到 1.45e7 Hz。

## 实施步骤

1. 落盘本文档。
2. `git checkout -b feat/rust-emitter origin/dev`。
3. Rust runtime（`Wire` + `PycReg`）。
4. RustEmitter + pycc `--emit=rust`。
5. counter/arith harness + pytest。
6. 性能脚本 + 把实测回写本文。
7. 更新 PIPELINE / 架构文档。

## 待确认事项

无。已拍板：首版功能仿真子集；先跑通 counter 等小程序，大型测试以后再加。后果：本 PR 能 `pycc --emit=rust`、用 harness 跑 counter、并给出 C++ vs Rust 的 Hz 表；不能跑 LinxCPU/LinxCore，也不能走完整 `pycircuit build` testbench。
