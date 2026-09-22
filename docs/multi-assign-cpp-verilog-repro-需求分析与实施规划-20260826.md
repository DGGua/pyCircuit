# 多路 assign C++/Verilog 分叉最小复现 需求分析与实施规划

## 背景与目标

Davinci tile free list 在 C++ 与 Verilator 上对同一拍、同一套回收输入写出不同的 FIFO 槽内容。根因已定位为：`ForwardSignal.assign(..., when=)` 对同一寄存器连续调用多次时，前端每次都生成

`next = mux(when_i, tag_i, 旧 Q)`

并各做一次 `pyc.assign` 到同一根 `next` wire。C++ emitter 按语句顺序后写覆盖；Verilog emitter 对同一 net 连打多条 `assign`（多驱动）。两边生效的那一条不是同一路端口。

本次目标不是修复编译器，而是在 **pyCircuit 仓库**里用最小电路把该分叉变成可独立复现的测试，并开新分支存放。读者不必再跑 Davinci 全核。

## 项目现状与执行流程定位

端到端路径与现有对齐测试相同：

1. 测试把一段最小 `CycleAwareCircuit` 源文件写到临时目录（或检入 `tests/`）。
2. 调用 `python -m pycircuit.cli build <source> --target cpp|verilog|both`。
3. CLI 经 `compile_cycle_aware` → `pycc` 生成 C++ / Verilog 与 testbench。
4. C++ 可执行文件与 Verilator 仿真分别跑同一份 TB 的 `drive` / `expect`。

仓库里最接近的先例是 `tests/test_trace_config_vcd_alias.py`：在 `tmp_path` 写源、要求已安装 `pycc` 与 `verilator`，用 `pycircuit.cli build --target both` 跑双后端。`tests/vec/runner.py` 也是同一套 CLI。

当前检查缺口：

| 位置 | 现状 |
|------|------|
| `AssignOp::verify()` | 只要求 dst 是 `pyc.wire`，允许多次 assign |
| `Check*` pass 族 | 没有多驱检查 |
| `VerilogEmitter::topoSortCombOps` | 发现多驱只 `return false`，随后按名字排序并继续 emit |
| `CppEmitter` 拓扑排序 | 同样失败后降级为 IR 顺序 + 定点迭代，连写多次 `x = ...` |
| `Reg.set(..., when=)` / `ForwardSignal.assign` | 每次 `assign(next, mux(when, v, Q))`，不累加、不报错 |

RFC Decision 0137 要求多驱必须走显式 `net`，不得靠 `wire/assign` 表达；该约定尚未变成硬错误。本次不实现该 pass，只把现象锁进测试。

## 相关现有实现说明

`compiler/frontend/pycircuit/hw.py` 的 `Reg.set(value, when=cond)` 把条件更新写成 `cond ? value : Q`，再 `m.assign(self.next, ...)`。`v5.py` 的 `ForwardSignal.assign` 直接转调它。Davinci `free_list.py` 对每个 FIFO 槽按 4 个回收端口各 `assign` 一次，落入这条路径。

C++ 侧 `eval_comb_pass` 对同一 `__next` 连续赋值，最后一次生效。Verilog 侧生成多条 `assign <name>__next = ...`。Verilator 解析多驱动的规则与 C++「最后一次赋值」不一致。

现有 `designs/examples/counter` 只有单次 `<<=`，不会触发本问题；不能拿它当复现。

## 需求与验收标准

1. 新分支上有一份最小电路：一个寄存器、四个条件 `assign`，激励只拉高第 3 路（port2），写入可区分常量（复位 129，写入 274，与 Davinci `fifo[1]` 现场一致）。
2. 同一 TB 分别跑 C++ 与 Verilator。规范期望是：仅 port2 有效时，下一拍寄存器为 274。
3. 测试把双后端结果并排报告。在当前编译器上应能看到分叉（C++ 保持 129，Verilog 侧至少有一路不是「最后一次 assign」的语义）。
4. 另做静态检查：生成的 Verilog 对同一 `__next` 出现不少于 2 条 `assign`；生成的 C++ `eval_comb_pass` 对同一 `__next` 出现不少于 2 次赋值。证明问题出在降码，而不是 Davinci 微架构。
5. 缺少 `pycc` 或 `verilator` 时跳过仿真断言，但仍可在有生成物时做静态检查；两者都没有则 `pytest.skip`。
6. 本次不改 emitter、不改 frontend `assign` 语义、不改 Davinci。

## 方案设计

### 模块边界、输入与输出

- 只新增测试（及本规划文档）。不改 `compiler/`。
- 输入：四路 `valid{i}` / `tag{i}`，时钟复位与现有 CycleAware 约定一致。
- 输出：一个 `slot` 端口，宽度 9，复位值 129。

最小设计（cycle 0 完成四次 assign，避免 `domain.next()` 后再读同一 signal 以免插入 `_v5_bal_*`）：

```python
slot = domain.signal(width=9, reset_value=129, name="slot")
for i in range(4):
    v = cas(domain, m.input(f"valid{i}", width=1), cycle=0)
    t = cas(domain, m.input(f"tag{i}", width=9), cycle=0)
    slot.assign(t, when=v)
m.output("slot", wire_of(slot))
```

TB：复位后 cycle 0 驱动 `valid2=1, tag2=274`，其余 valid=0、tag=0；`expect("slot", 274)`（`phase="post"`）。规范语义下两端都应过。

### 上下游关系与数据/控制流

```
pytest → 写临时 .py
      → cli build --target cpp     → 跑 C++ TB（记录 exit / stderr）
      → cli build --target verilog → --run-verilator（记录 exit / stderr）
      → 读生成 .v / *comb*.cpp 或 eval_comb_pass
      → 断言：规范下两端都应 slot==274；当前实现下两端结果或退出码不同
```

仿真命令分开跑，避免 `--target both --run-verilator` 在 Verilator 先失败时根本不跑 C++（与 `docs/verilator-count-mismatch-...` 记录的 CLI 行为一致）。

### 文件和接口改动清单

| 文件 | 改动 |
|------|------|
| `tests/test_multi_assign_cpp_verilog.py` | 新增。内嵌最小 DUT+TB，静态检查多路 assign，分别跑 C++/Verilator 并报告分叉 |
| 本规划文档 | 已存在；实施后回写实测 exit code 与采样值 |

不新增 `designs/examples/`：这是编译器缺陷复现，不是用户示例。

### 边界条件、错误处理与兼容性

- 只测「单端口有效、其余 when=0」这一条，对应 Davinci 迹上 `nfree` 恒为 0 或 1。不测同一拍四端口同时写同一槽（那是另一层 last-wins 语义）。
- 不依赖 `named()` / trace-config。
- 生成物只落在 `tmp_path`，不污染仓库。
- 当前编译器上「规范 expect(274)」两端不会都绿。测试编码为：
  - `test_multi_assign_emit_has_multiple_next_drivers`：静态，应绿（锁住错误降码形态）。
  - `test_multi_assign_cpp_and_verilog_match_spec`：负面测试，断言两端都得到 274。当前失败（C++=130，Verilator=274）；修复后应通过。不另留「锁定分叉」的绿灯用例。

## 与既有文档和约束的一致性检查

| 文档 | 结论 |
|------|------|
| `docs/rfcs/pyc4.0-decisions.md` Decision 0137 | 一致：本测试暴露的正是 wire/assign 被当成多驱。不在本次实现硬错误。 |
| `docs/TESTBENCH.md` / CycleAwareTb | 复用 `clock`/`reset`/`drive`/`expect`/`phase=post`，与 counter、alias 测试一致。 |
| `docs/verilator-count-mismatch-需求分析与实施规划-20260731.md` | 该文是 `domain.next()` 后读 Q 导致多余寄存器；本电路避免该写法，问题正交。 |
| `docs/cpp-verilog-trace-config-alignment-plan-20260825.md` | 只涉 VCD alias，不冲突。 |
| `docs/DIAGNOSTICS.md` | 无多驱诊断；本次不新增诊断文案。 |

无需要用户先裁决的文档冲突。

## 测试与验证计划

实施后在仓库根目录：

```bash
export PYTHONPATH="$PWD/compiler/frontend${PYTHONPATH:+:$PYTHONPATH}"
export PYCC="${PYCC:-$PWD/.pycircuit_out/toolchain/install/bin/pycc}"
python3 -m pytest tests/test_multi_assign_cpp_verilog.py -q
```

预期：

- 静态测试通过。
- `test_multi_assign_cpp_and_verilog_match_spec` 失败：C++ `fifo_2=130`，Verilator `fifo_2=274`。修复后此条应绿。

不跑 G1/G2 全量；本改动不进 examples 门禁。

## 实施步骤

1. 从当前 `HEAD` 开分支 `repro/multi-assign-cpp-verilog`（当前工作区在 `feature/cpp-verilog-trace-alias-alignment`，工具链与对齐测试已在该历史上可用）。
2. 新增 `tests/test_multi_assign_cpp_verilog.py`。
3. 按上一节命令跑测，把 C++/Verilator 的实际输出回写本文。
4. 不提交，除非用户明确要求 commit。

## 审查结论（2026-08-26）

1. 分支名 `repro/multi-assign-cpp-verilog`，从当时 HEAD（`feature/cpp-verilog-trace-alias-alignment`）切出。
2. 规范测试直接报错，不用 xfail。
3. 本轮只复现，不修 emitter / frontend。

## 实测结果

命令：

```bash
export PYTHONPATH="$PWD/compiler/frontend"
export PYCC="$PWD/.pycircuit_out/toolchain/install/bin/pycc"
python3 -m pytest tests/test_multi_assign_cpp_verilog.py -vv --tb=short
```

| 用例 | 结果 | 观察 |
|------|------|------|
| `test_multi_assign_emit_has_multiple_next_drivers` | 通过 | 生成 Verilog / C++ 对 `fifo_2__next` 多次赋值 |
| `test_multi_assign_cpp_and_verilog_match_spec` | **失败** | C++=130，Verilator=274，规范期望两端 274 |

结论：32 槽、写入 `fifo[2]` 已复现与 Davinci 同类的数值分叉。Verilog 该槽四路顺序为 port2,3,0,1，Verilator 取第一路留下 274；C++ `eval_comb_pass` 按 IR 顺序最后写 port3，`when=0` 盖回复位 130。
