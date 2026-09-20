# Rust emitter 细粒度 wire 控制码探索

日期：2026-08-31  
状态：按已拍板方向实施（探索第一步）  
分支：`feat/rust-emitter`

## 背景与目标

Rust emitter 首版把每根线都做成 `Wire<W>`（内部一个 `u64`，每次运算都 `Wire::new` 再掩码）。microbench 上这比 C++ 慢大约 7 倍。用户要求探索更细的 wire 控制码：按宽度选存储，并且只在会涨出宽度的运算后截断。

**目标**：生成代码用 `bool/u8/u16/u32/u64` 直接算；`and/or/xor` 不再多余掩码；`add/sub/shl/mul` 仅在宽度不是存储宽度时掩码。不改 MLIR 语义，不改 C++。

## 项目现状与执行流程定位

`pycc --emit=rust` → `RustEmitter` 生成模块 `struct` → harness/`rustc` 编译。改前字段类型是 `Wire<8>`，运算是 `self.a + self.b`。C++ 对照路径不动。

## 相关现有实现说明

- `compiler/mlir/lib/Emit/RustEmitter.cpp`：按宽度生成 `bool`/`u*` 与内联表达式。
- `runtime/rust/src/bits.rs`：只保留 `slt`/`ashr`/`sext`/除法 helper。
- `runtime/rust/src/reg.rs`：`PycReg<T>`，控制脚 `bool`。
- `tests/rust_emit/harness/*_main.rs`：端口用 `bool`/`u*`。

## 需求与验收标准

1. 生成字段：`i1`/`clock`/`reset` → `bool`；2..=8 → `u8`；9..=16 → `u16`；17..=32 → `u32`；33..=64 → `u64`。
2. 组合运算写成原生整数/`bool` 表达式，不再经过 `Wire` 运算符。
3. 宽度等于存储宽度时，`add/sub` 只用 `wrapping_*`，不再 `& mask`；宽度如 19 存在 `u32` 里时才掩码。
4. `and/or/xor` 不掩码（操作数已被截断）。
5. counter/arith 功能与 C++ 仍一致；mem 仍拒绝。
6. 重跑 `run_rust_vs_cpp.py`，把 microbench Hz 与上一轮基线（C++ 8.79e7、Rust 1.24e7）对比并写回。

## 方案设计

### 模块边界、输入与输出

Emitter 按 SSA 宽度选 Rust 存储类型并内联表达式。Runtime 只保留 `PycReg<T>` 和少量有符号/移位 helper（`ashr`/`slt`/`sdiv` 等）。Harness 改为给 `bool`/`u*` 赋值。

### 上下游关系与数据/控制流

Pass 管线不变。只改 Rust 生成形态与 runtime/harness。C++ 发射与 G2 不动。

### 文件和接口改动清单

- `compiler/mlir/lib/Emit/RustEmitter.cpp`：类型与运算发射。
- `runtime/rust/src/{lib,bits,reg}.rs`：`PycReg<T>` + helper；`Wire` 可保留给单测或缩小。
- `tests/rust_emit/harness/*_main.rs`：端口改原生类型。
- 本文档与首版规划的实测回写。

### 边界条件、错误处理与兼容性

宽度 1 的加减按模 2（即异或）。有符号移位/除法走 helper，避免生成错误的符号扩展。生成代码仍是实验性 API，无稳定 ABI。

## 与既有文档和约束的一致性检查

| 文档 | 结论 |
|------|------|
| rust-emitter 20260830 规划 | 子集范围不变，只改生成形态 |
| pyc4.0 语义在 MLIR | 不改 dialect/pass |
| Decision 0001 tick/transfer | `PycReg` 仍两阶段 |

## 测试与验证计划

```bash
python3 -m pytest tests/rust_emit/test_rust_emitter.py -q -k 'not test_perf'
python3 flows/tools/perf/run_rust_vs_cpp.py --repeats 3
```

基线（改前，2026-08-30）：microbench Rust 1.24e7 Hz。本探索记录改后中位数，不设必须超过 C++ 的门槛。

### 实测结果（2026-08-31，narrow wires）

环境与脚本同 20260830：`g++ -O2`，`rustc -C opt-level=2`，每项重复 3 次取中位数。JSON：`.pycircuit_out/perf/rust_vs_cpp.json`。

| 设计 | 周期 | C++ Hz（中位） | Rust Hz（中位） | 相对 08-30 Rust |
|------|------|----------------|-----------------|-----------------|
| counter | 2e6 | 1.08e8 | 4.00e9 | 仍被 rustc DCE，不可比 |
| arith | 5e6 | 9.98e8 | 2.00e9 | 仍被 rustc DCE，不可比 |
| microbench（32 级 add/mux/reg） | 2e5 | 8.82e7 | **1.45e7** | 1.24e7 → 1.45e7（约 +17%） |

读数说明：

- 功能：`pytest tests/rust_emit/test_rust_emitter.py -k 'not test_perf'` 4 passed；counter `count=1..5` 与 C++ 一致。
- 生成形态：counter 为 `clk: bool`、`count: u8`、`wrapping_add`、`if enable { ... }`、`PycReg<u8>`；arith 19-bit 为 `u32` 上 `(a.wrapping_add(b)) & 0x7FFFFu32`。
- microbench 仍比 C++ 慢约 **6.1×**（08-30 约 7.1×）。这一步去掉了 `Wire` 装箱，但全量 `eval`、每个 SSA 都写成 struct 字段，仍然很重。
- counter / arith 的 Rust Hz 仍不可当作 ISA 对比。
- Rust 二进制仍约 4.5 MB（链 `std`）；C++ 约 19 KB。

后续（同日）：对照 `rustc` 改为 `-C opt-level=3`，见 `docs/rust-emitter-opt-level3-需求分析与实施规划-20260831.md`。microbench Rust 中位 1.45e7 → 1.48e7，几乎不变。

## 实施步骤

1. 落盘本文。
2. 改 runtime 与 emitter。
3. 改 harness。
4. 功能测试 + 性能对比 + 回写数字。已完成。

## 待确认事项

无。已拍板：按宽度选 `bool/u8/u16/u32/u64`，只在会涨出宽度的运算后掩码；先探索这一步，不打包 1-bit。后果：生成代码和 harness 不再使用 `Wire<W>` 作为 DUT 字段类型；C++ 路径不变。
