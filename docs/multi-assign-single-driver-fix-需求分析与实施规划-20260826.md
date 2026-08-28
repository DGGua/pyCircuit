# 多路 assign 单驱动修复 需求分析与实施规划

## 背景与目标

`ForwardSignal.assign(..., when=)` / `Reg.set(..., when=)` 对同一寄存器连续调用时，每次都生成

`next = mux(when, value, Q)`

并各做一次 `pyc.assign` 到同一根 `next`。C++ 按语句顺序后写覆盖；Verilog 对同一 net 打出多条 `assign`，Verilator 取文件里的第一路。`when=0` 的后写会把已经算好的写入盖回旧 Q，两端还可能选出不同的一路。

复现已锁定：`tests/test_multi_assign_cpp_verilog.py::test_multi_assign_cpp_and_verilog_match_spec`。32 槽 free list、只拉高 port2 写 274 到 `fifo[2]` 时，C++ 得 130、Verilator 得 274。规范期望两端都是 274。该用例是负面测试，修复后应绿。

目标：让连续 `assign(..., when=)` 的硬件含义与 Python 循环顺序一致，并且 C++ / Verilog 只留下**一条** next 驱动，从而两端行为一致。本次不修 Davinci 指针 9bit 在 512 回绕、FIFO 深度 384 的问题（独立缺陷）。

## 项目现状与执行流程定位

用户写法：

```python
for i in range(4):
    fifo[s].assign(tags[i], when=hit & valids[i])
```

`v5.py` 的 `ForwardSignal.assign` 转到 `Reg.set`。`hw.py` 每次：

```python
m.assign(self.next, when_w._select_internal(value, self.q))
```

`dsl.py` 的 `assign` 只往 MLIR 再追加一条 `pyc.assign`，不替换旧驱动。

之后：`compile_cycle_aware` → `pycc` → C++ `eval_comb_pass` 连写 `__next`；VerilogEmitter 发现多驱只放弃拓扑排序，仍 emit 四条 `assign`。RFC Decision 0137 要求多驱必须走显式 `net`，`wire/assign` 不得当多驱用；该约定目前不是硬错误。

正确语义（与循环顺序一致）：

```text
n = Q
n = mux(when0, v0, n)
n = mux(when1, v1, n)
n = mux(when2, v2, n)
n = mux(when3, v3, n)
next = n
```

`when=0` 保持上一端口的结果，而不是退回 Q。同一槽两路同时命中时，后写的端口生效。

## 相关现有实现说明

| 组件 | 路径 | 现状 |
|------|------|------|
| 条件写入口 | `compiler/frontend/pycircuit/hw.py` `Reg.set` | else 永远是 Q，每次新 `assign` |
| V5 语法 | `compiler/frontend/pycircuit/v5.py` `ForwardSignal.assign` | 转调 `Reg.set` |
| IR 追加 | `compiler/frontend/pycircuit/dsl.py` `assign` | 只 emit，不合并 |
| C++ 生成 | `compiler/mlir/lib/Emit/CppEmitter.cpp` | 多驱则拓扑失败，按 IR 顺序连写，最后一次留下 |
| Verilog 生成 | `compiler/mlir/lib/Emit/VerilogEmitter.cpp` | 多驱则拓扑失败，仍输出多条 `assign` |
| 复现测试 | `tests/test_multi_assign_cpp_verilog.py` | 规范用例当前红 |

规格文档（`docs/PyCircuit_V5_Spec.md`、`docs/v6_PyCircuit_Specification.md`）把 `sig.assign(expr, when=cond)` 写成「条件赋值 / 使能」。多处示例是循环里对同一存储 `assign(..., when=hit)`，与 free list 同类。用户预期是「使能时写入，否则保持**已经决定的 next**」，不是「否则强制回到旧 Q」。

## 需求与验收标准

1. 连续 `assign(..., when=)` 按调用顺序累加；`when=0` 不撤销先前有效写。
2. 同一 `reg.next` 在生成的 C++ / Verilog 里只有一个驱动。
3. `tests/test_multi_assign_cpp_verilog.py::test_multi_assign_cpp_and_verilog_match_spec` 通过（两端 `fifo_2=274`）。
4. 单次 `assign(..., when=)` 行为不变：不使能则保持 Q。
5. 无条件 `<<=` / `set(v)` 覆盖此前挂起的条件写（整拍 next 被替换）。
6. 若仍存在一线多驱（例如用户对同一 `pyc.wire` 连写两次 `m.assign`），Verilog/C++ emit **报错退出**，不再降级继续生成。
7. 不改 Davinci 仓库；free_list 现有循环写法应直接变正确。
8. 不在本方案改 TFL 指针取模（环长 512 vs 深度 384）。

## 方案设计

比较过的做法：

| 做法 | 优点 | 缺点 | 结论 |
|------|------|------|------|
| A. 前端累加，并且对 `r.next` 只保留一条 `pyc.assign` | 语义改在 `when=` 定义处；Davinci 与 Spec 循环不用改 | 要处理「再次 set」替换旧驱动 | **推荐** |
| B. 只改 Verilog 取最后一条 assign | 改动面小 | 最后一条仍是 `mux(c3,v3,Q)`，port2 有效时仍丢写；C++ 现况也不对 | 否 |
| C. 禁止二次 assign，逼用户手写一条 mux | 与 Decision 0137 字面一致 | 打断 free_list 和 Spec 里的循环写法 | 仅作为残留多驱的后盾，不当主路径 |
| D. 只做 MLIR 合并 pass | 能兜住漏网的 `pyc.assign` | `mux(c,v,Q)` 才能可靠识别；语义本该属于 frontend | 可选加固，不单独作为主修 |

推荐 **A 为主，C 为发射期硬错误，D 仅在有第二来源时再加**。

### 模块边界、输入与输出

只动 pyCircuit 前端（及必要时 emitter 诊断）。输入仍是现有 `Reg.set` / `ForwardSignal.assign` / `<<=`。输出是单一 next 表达式接到寄存器 D。

`Reg` 增加「本拍已决定的 next」：

1. 第一次条件写：`pending = mux(when, value, Q)`，`assign(next, pending)`。
2. 再一次条件写：`pending = mux(when, value, pending)`，**替换**对 `next` 的那条 assign（或先不 emit assign，在寄存器关闭/编译刷出时写一次）。
3. 无条件 `set(v)` / `<<= v`：`pending = v`，同样只保留一条 assign。

推荐实现不要改 `dsl.Module.assign`（它只追加文本 IR，不便替换旧行），也不要每次 `set` 都立刻 `m.assign`。

现成密封钩子已经有了：`Module.add_finalizer` 在 `emit_func_mlir()` 拼 body **之前**跑（`RvQueue` 就是用它保证 push/pop 各只一条驱动）。Davinci 与 CLI 都走 `compile_cycle_aware(..., eager=True)`，最终都会 `emit_mlir()`，因此 finalizer 覆盖复现测试和 Davinci 主路径。

`Reg` 是 `frozen` dataclass，不能直接挂可变字段。待定 next 放在所属 `Circuit` 上，按 `r.next` 的 signal ref 建表：

1. 第一次 `Reg.set`：登记 `pending = mux(when, value, Q)`（无条件则 `pending = value`），并对该 `Circuit` 注册一次 finalizer（整模块共享一个 flush，或按寄存器各注册一次，效果相同）。
2. 再次 `Reg.set`：只更新 Python 侧 `pending`。条件写的 else 是**上一份 pending**，不是 Q；无条件写直接换成新值。
3. `emit_mlir` 时 finalizer 对每个被 `set` 过的 `r.next` 恰好执行一次 `m.assign(self.next, pending)`。

这样生成的 MLIR 里每个 `__next` 只有一条 `pyc.assign`，C++ 后写覆盖和 Verilog 第一路生效都不再有第二路可分歧。

不要采用「每次仍 assign、靠 MLIR 只留最后一条」作为主路径：若 Verilog 仍 emit 前面的 `assign`，Verilator 继续 first-wins，语义仍然错。MLIR 合并 pass（方案 D）只作为漏网加固，本次不做。

### 上下游关系与数据/控制流

```
用户 assign/set
  → Reg 累加 pending next（else=上一 pending 或 Q）
  → 密封：对 r.next 恰好一条 pyc.assign
  → pycc
  → C++ / Verilog 各一个 __next 驱动
```

残留的一线多 `pyc.assign`（不是 Reg.set 路径）：emitter 报错。

### 文件和接口改动清单

| 文件 | 改动 |
|------|------|
| `compiler/frontend/pycircuit/hw.py` | `Circuit` 增加 pending-next 表与 flush finalizer；`Reg.set` 只更新 pending，不再每次 `m.assign` |
| `compiler/frontend/pycircuit/v5.py` | `ForwardSignal.assign` / `<<=` 已转调 `Reg.set`，语义不必再改 |
| `compiler/mlir/lib/Emit/VerilogEmitter.cpp` | 多驱由「拓扑失败继续 emit」改为 `emitError` |
| `compiler/mlir/lib/Emit/CppEmitter.cpp` | 同样改为硬错误（避免再靠后写覆盖掩盖问题） |
| `tests/test_multi_assign_cpp_verilog.py` | 保持规范用例；修复后应变绿。可补：单次 when 保持 Q、二次 when=0 保留第一次写、两路同时命中后写生效、生成 Verilog 对 `__next` 只有一条 assign |
| `docs/PyCircuit_V5_Spec.md` / 教程（建议同批或紧随） | 写明连续 `assign(..., when=)` 按顺序累加 |

不改 Davinci `free_list.py`。

### 边界条件、错误处理与兼容性

- 同一拍两路 `when=1` 写同一寄存器：后调用的值生效。
- 先条件写再无条件写：无条件覆盖。
- 先无条件再条件写：条件写的 else 是无条件值，不是旧 Q。
- 对普通 `pyc.wire` 多次 `m.assign`：编译失败，提示改成一条 mux 或使用 `Reg.set` 累加。
- 依赖「后一次 `when=0` 把 next 打回 Q」的设计（若存在）会改变行为；那是当前 C++ 的错误语义，Verilog 本就不稳定。视为缺陷修复，不保留旧行为开关。

## 与既有文档和约束的一致性检查

| 文档 | 结论 |
|------|------|
| `docs/rfcs/pyc4.0-decisions.md` Decision 0137 | 一致：修复后 `wire/assign` 恢复单驱动；真多驱仍须显式 net。发射期硬错误落实该 decision。 |
| `docs/PyCircuit_V5_Spec.md` `assign(..., when=)` | 把「保持」解释为保持已决定的 next，与循环示例一致。 |
| `docs/multi-assign-cpp-verilog-repro-需求分析与实施规划-20260826.md` | 本方案是其后续修复；验收用已有负面测试。 |
| `docs/verilator-count-mismatch-需求分析与实施规划-20260731.md` | 正交（`domain.next()` 后读 Q 插 balance）。实施时避免在 `set` 里再读错 cycle。 |
| `docs/DIAGNOSTICS.md` | 新增多驱硬错误文案时应补一条；可与实施一并写。 |

无必须先裁决的文档冲突。默认不保留旧「else=Q 且多驱」兼容开关。

## 测试与验证计划

修复后：

```bash
export PYTHONPATH="$PWD/compiler/frontend"
export PYCC="$PWD/.pycircuit_out/toolchain/install/bin/pycc"
python3 -m pytest tests/test_multi_assign_cpp_verilog.py -vv --tb=short
```

预期：`test_multi_assign_cpp_and_verilog_match_spec` 通过；若保留 `test_multi_assign_emit_has_multiple_next_drivers`，应改为断言 **恰好一条** `fifo_2__next` 驱动，或删除该静态用例。

建议新增小型测试（可与规范用例同文件或 `tests/v5/`）：

- 只 `assign(v, when=0)` → 保持复位值，两端一致。
- 先 `assign(274, when=1)` 再 `assign(0, when=0)` → 仍为 274。
- 先 `assign(100, when=1)` 再 `assign(200, when=1)` → 200。

回归：现有 `tests/test_trace_config_vcd_alias.py`（单次路径）应不受影响。Davinci 对齐不在本仓库跑；修复后可用原 prefix-1143 做一次对照（可选，不作为本 PR 门禁）。

## 实施步骤

1. 改 `Reg.set`：维护 pending next，密封后只留一条 `pyc.assign`。
2. Verilog/C++ emitter：一线多驱改为编译错误。
3. 调整复现测试的静态断言（多驱 → 单驱）；规范仿真应变绿。
4. 补小型 when= 组合用例；按需改 Spec 一句语义说明。
5. 把实测结果回写本文。

## 实施结果（2026-08-26）

已按批准方案落地：`Reg.set` 只更新 `Circuit` 上的 pending next，`emit_mlir` 的 finalizer 对每个 `r.next` 只 `assign` 一次；Verilog/C++ emitter 对残留多驱 `emitOpError` 后退出。未改 Spec/教程，未跑 Davinci prefix-1143。

```text
export PYTHONPATH="$PWD/compiler/frontend"
export PYCC="$PWD/.pycircuit_out/toolchain/install/bin/pycc"
python3 -m pytest tests/test_multi_assign_cpp_verilog.py -vv --tb=short
```

结果：5 passed（约 10s）。规范用例两端 `fifo_2=274`；生成物每个 `__next` 一条驱动；`when=0` / 后写覆盖组合用例通过。

后续补充（同日）：`Circuit.assign` 也进入同一 pending 表，后写覆盖；flush 放在全部 finalizer 之后，避免 RvQueue 晚到的 assign 漏刷。IR 残留多驱仍由 emitter 硬错误挡住。`test_raw_double_assign_*` 改为断言两端 `o=2`。

## 待确认事项

1. 是否同意以 **前端累加 + 单驱动** 为主修，发射期对残留多驱硬错误？（推荐：是）
   **已确认（2026-08-26）：是。**
2. 是否在本 PR 改 Spec/教程表述，还是只改代码和测试？
   **已确认：不改 Spec / 教程。**
3. 修复后是否需要顺带在 Davinci 上复跑 prefix-1143（本仓库外，可选）。
   **未要求，本次不做。**
