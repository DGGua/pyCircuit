# 保留状态身份并在优化中重映射观测视图需求分析与实施规划

## 背景与目标

当前 C++ 状态优化默认先跑 `pyc-strip-state-observability`：从 `pyc.reg`、`pyc.delay_line` 以及指向它们的 `pyc.alias` 上删除 `pyc.name`、`pyc.debug_keep`、`pyc.observable`、`pyc.probe*`、`pyc.trace*`，再删掉已经没有 SSA 使用的状态 alias，然后用 `pyc-eliminate-dead-state` 清掉只靠这些标记活着的寄存器。

这样做的原因是后面的合并、收链、retiming、packing 把上述属性当成硬边界。V5 前端的 `signal()` / `cycle()` 几乎总会通过 `Circuit.out()` 给 `q` 挂上 `pyc.alias { pyc.name = "..." }`（用户名或自动 `_v5_reg_N`）。不先剥名字，structural 优化对普通用户寄存器基本空转。

用户要求改成：

1. 删除 `pyc-strip-state-observability` 这个 pass。
2. 删除 `pycc` 的 `--state-opt-preserve-observability`，也不再提供“剥身份 / 钉死身份”两种模式。
3. 所有身份属性都留在 IR 里。
4. 后面的等价合并、delay-line 收链、pipeline retiming、lane packing 仍然按功能证明做优化，不再因为身份而跳过候选。
5. 优化删掉或换掉物理对象之后，若该身份需要留给外部读取（probe 清单、`dut.read`、VCD、`ProbeRegistry`），单独拉出一条只读视图：一根线（`pyc.alias`）、一个 `pyc.delay_line` 的 `q`、或一个 `pyc.delay_tap`。

目标是同时保住 C++ 状态对象/调度收益，以及外部仍能按原名字读到**同一周期、同一位宽的值**。

工作区里已有未提交改动：新加状态优化只在 `--emit=cpp` 运行。本方案叠在该门控之上，不回头改 Verilog / `--emit=none` 的网表。

## 项目现状与执行流程定位

`pycc` 在 `compiler/mlir/tools/pycc.cpp` 里组装 PassManager。与本次相关的 C++ 路径当前是：

```text
eliminate-wires
eliminate-dead-state
        │
        ├─ 未开 --state-opt-preserve-observability：
        │     strip-state-observability
        │     eliminate-dead-state          ← 观测-only 状态在这里被删
        ▼
combine-delay-chains (merge-only)
canonicalize + CSE
combine-delay-chains (merge-only, cascade)
retime-pipelines
eliminate-dead-state
combine-delay-chains (form delay-line / tap / share)
pack-state-lanes
        │
pack-i1-regs                                 ← 仅非 preserve 模式
fuse-comb                                    ← 稳定名字是 fusion barrier
canonicalize / CSE / remove-dead-values
cpp-placement                                ← 带 pyc.name 的值留在 C++ struct
emitter / probe-manifest
```

`--emit=verilog` 与 `--emit=none` 已门控为不跑这组新加改写（未提交）。`pyc-opt` 仍可单独调用 `pyc-strip-state-observability`、以及各 pass 的 `preserve-observability` 选项。

前端名字怎么来：

- `compiler/frontend/pycircuit/v5.py` 的 `signal()` / `cycle()` 一定会给寄存器一个名字，再走 `Circuit.out()`。
- `compiler/frontend/pycircuit/hw.py` 的 `out()` 先建匿名 `pyc.reg`，再对 `q` 做 `alias(..., name=fullname)`。`pyc.name` 通常在 alias 上，不在 `pyc.reg` 上。
- 周期对齐插入的寄存器带 `pyc.generated = "cycle_balance"` 和 `_v5_bal_*` 名字。probe 清单和 C++ `ProbeRegistry` 已显式忽略这类名字。

后端怎么用名字：

- `pycc` 的 probe manifest 遍历带 `pyc.name` 且单结果的 op，能追到 reg / delay-line / delay-tap 则标 `kind=state`。
- `CppEmitter` 同样收集 `pyc.name`，对状态调用 `addReg` / `addRegSlice`，需要 `q`、`pending`、`qNext`。
- `CppPlacementPass` 把带 `pyc.name` 的 SSA 钉在 C++ 结构体上，避免变成局部变量后探针地址失效。

## 相关现有实现说明

### `pyc-strip-state-observability`

文件：`compiler/mlir/lib/Transforms/StripStateObservabilityPass.cpp`。

只删观测属性并擦掉无使用的状态 alias，不改功能连接。统计写在 function 上：`pyc.stats.state_opt_observability_attrs_stripped`、`pyc.stats.state_opt_observation_aliases_removed`。

### 身份如何钉死优化

`compiler/mlir/lib/Transforms/StateOptimization.cpp` 里：

- `hasStableStateName`：有 `pyc.name` 且不是 `cycle_balance`。
- `shouldKeepStateOptimization`：`pyc.debug_keep`，或带 `pyc.probe*` / `pyc.trace*` / `pyc.observable`。
- `StateObservabilityAnalysis`：状态本身或它 `q` 后面的 alias 链具备上述身份，就把这颗状态标为 pinned。

`preserveObservability=true` 时：

- 等价合并跳过 pinned 寄存器；
- 带稳定名字的 alias 不透明，收链在这里断开；
- 中间只读 fanout 不允许改写成 tap；
- retiming 的组合锥和共同 delay 下沉遇到身份就失败；
- packing 跳过 pinned lane，只有「名字打在 reg 本身、alias 上没有名字」这一条窄例外，会把名字挪到 extract 后的 alias，供 `addRegSlice` 使用。

`preserveObservability=false` 时上述钉钉不生效，但默认管线会先 strip，身份已经不在了。

### 各改写实际删掉什么

- **等价合并**（`CombineDelayChainsPass.cpp` 的 `mergeEquivalentStates`）：删掉重复 `pyc.reg`，使用接到 survivor 的 `q`。不保留 victim 的名字。
- **收链**：一串 `pyc.reg` 换成一条 `pyc.delay_line`，中间 alias 擦掉。已有功能侧读时才会插 `pyc.delay_tap`。
- **共享 delay-line**：等价 history 并到 survivor，victim 删除。
- **pipeline retiming**（`RetimePipelinesPass.cpp`）：多级带组合锥的流水线收成一条 delay-line；有外部使用的中间级用 `delay_tap` 加重放前缀。没有外部使用、只带着名字的中间级不会单独留视图。
- **共同 delay 下沉**：`f(D^N(x), D^N(y))` 变成 `D^N(f(x,y))`。源 history 删除，逻辑 bits 减少。原 `x`/`y` 的中间值不再存在，除非再留一份 history。
- **packing**：多条同控制 lane 拼成一个宽 `pyc.reg` / `pyc.delay_line`，原 lane 用 `extract` 替换。已能把 reg 上的单个 `pyc.name` 挂到 slice alias。
- **`pyc-pack-i1-regs`**：旧 i1 packer，会重建 alias 并拷贝属性，但默认只在非 preserve 模式运行。

### 无使用观测视图会被后续 pass 删掉

`pyc.alias` 和 `pyc.delay_tap` 在方言里标了 `Pure`。只为 probe 拉出来、没有其它 SSA 使用者的 alias/tap，会被后面的 `canonicalize` / `remove-dead-values` 当死代码删掉。probe 清单靠「带 `pyc.name` 的 op 还在」工作，因此重映射后的视图必须能活到 emit。

`pyc-eliminate-dead-state` 只看 `debug_keep` 和结果是否还有使用。若观测 alias 还在并读着 `q`，对应寄存器不会被删。

## 需求与验收标准

1. `pycc --help` 和 `pyc-opt --help` 不再出现 `--state-opt-preserve-observability`、`preserve-observability`、`pyc-strip-state-observability`。传入这些选项按 LLVM 未知选项失败。
2. `compiler/mlir/lib/Transforms/StripStateObservabilityPass.cpp` 删除，不再注册、不再链入管线。
3. `--emit=cpp` 仍跑合并 / 收链 / retiming / packing（以及现有预算开关）。身份不再让候选直接失败。
4. `--emit=verilog` / `--emit=none` 仍不跑这组新加改写（维持当前未提交门控）。
5. 被优化掉的状态若带**外部可读身份**，IR 中必须留下一条只读视图，且 `pyc.name`（以及其它观测属性）仍能被 probe manifest 和 `CppEmitter` 看见。
6. 视图的值与优化前该身份在同一周期看到的位模式一致；模块端口、周期数、reset/enable/init 功能语义不变。
7. `_v5_bal_*` / `pyc.generated = "cycle_balance"` 仍不是外部探针，不强制为它们拉视图。
8. `debug_keep` 与 name/probe 相同：只保证可读视图，不钉死物理 flop。与功能状态等价的观测副本可以合并，名字挂到 survivor。独一无二、不可重构的观测状态留下物理对象。
9. 共同 delay 下沉必须同时满足：功能结果改为 `D^N(f(...))`；每个带外部可读身份的源仍能按原名读到下沉前同一周期的值（留下原 history，不把名字改挂到 `f` 的结果上）。
10. 无使用的观测视图不得被 DCE / canonicalize 在 emit 前删掉。
11. 现有 delay-line / retiming / packing smoke 按新契约改写后通过；`xz_value_model_smoke` 一类按名字 `dut.read` 的夹具在 `--emit=cpp` 优化后仍能解析到该名字。

## 方案设计

### 模块边界、输入与输出

新增共享辅助（放在 `StateOptimization.cpp` / `.h`，供 combine / retime / pack 使用），不新增大 sweep 用的分析-only pass。

**外部可读身份**（需要拉视图）定义为，出现在 `pyc.reg`、`pyc.delay_line`、`pyc.delay_tap` 或指向状态的 `pyc.alias` 上的：

- 非 `cycle_balance` 的 `pyc.name`；
- `pyc.probe*`、`pyc.trace*`、`pyc.observable`；
- `pyc.debug_keep`（按「保留可读视图」理解，不再表示「禁止改写这颗 flop」）。

`cycle_balance` 的自动名继续排除。

每个身份记录为一条 `ObservationAttachment`：来源 op、它当时读到的 SSA、属性字典、以及它相对某条 history 的深度（若适用）。

改写删除或替换物理对象时，按下面规则物化视图，并把原属性拷到视图 op 上：

| 优化后值还在哪 | 拉出的视图 |
|---|---|
| 与 survivor / packed extract / 重放前缀同一拍的 SSA | `pyc.alias`（一根线），拷贝身份属性 |
| 紧凑 history 的中间槽 | `pyc.delay_tap`（按原深度），必要时再 alias 挂多个名字 |
| 紧凑 history 的末端 `q` | 名字挂在 `pyc.delay_line` 上，或多个 alias 指向它的 `q` |
| 共同 delay 下沉后的具名源 `x`/`y` | 功能侧改用下沉结果；源 history 留下当只读观测，身份仍挂在源 `delay_line`/`reg`/`tap` |
| 没有任何剩余状态能算出原值 | 不删这颗状态，身份留在原对象或它的 alias 上 |

同一值上的多个名字各留一条 alias。probe 清单本来就按 `field_path` 去重。

`pending` / `qNext`：视图若仍能经现有 `findRegQFromValue` 追到 survivor / delay-line / tap，C++ 继续注册为 state probe。这覆盖合并、收链、packing、以及 retiming 里已有 tap 的中间级。

### 上下游关系与数据/控制流

删除 strip 及其后那次「为剥身份服务」的 dead-state。C++ 路径变为：

```text
eliminate-wires
eliminate-dead-state
combine (merge-only)          ← 合并 + 把 victim 身份挂到 survivor.q 的 alias
canonicalize / CSE
combine (merge-only cascade)
retime-pipelines              ← pipeline 为带身份的中间级补 tap/线；共同下沉留下具名源 history
eliminate-dead-state          ← 不得删掉仍被观测视图使用的状态
combine (form / tap / share)  ← 收链时中间身份改挂 delay_tap
pack-state-lanes              ← 所有外部身份都走 slice alias，不只是 reg 上的单个 name
pack-i1-regs                  ← 始终运行；已有 alias 重建逻辑，补上「名字在 reg 上」的 slice alias
fuse-comb                     ← 观测视图继续作为 barrier，保证名字留在 struct
canonicalize / CSE / DCE      ← 观测视图必须在此存活
placement / emit / probe-manifest
```

`preserveObservability` 从 `createCombineDelayChainsPass` / `createRetimePipelinesPass` / `createPackStateLanesPass` 和 `pyc-opt` 选项里删除。`generated` 模式仍只改写带 `cycle_balance` 的候选，但不再因为观测属性跳过；若这类 op 上偶尔有外部身份，同样拉视图。

`StateObservabilityAnalysis` 的 pinned 不再用于「禁止优化」。可改成「收集 attachment」，或由各 rewrite 在删除前现场收集。推荐现场收集，避免分析与改写不同步。

共同 delay 下沉（已拍板：选项 B）：功能路径仍改写成 `D^N(f(x,y))`。带外部可读身份的源 history **不删**，只从功能消费者上断开，作为只读观测 delay-line / tap 留下；名字继续挂在这条源 history（或其中间 tap）上。`dut.read` 仍读到原来的 `x`/`y` 位模式。没有外部身份、且下沉后不再有使用的源照常删除，继续拿 bit 收益。实现上优先「留下原对象」，不为观测再 clone 一份相同 history。pipeline retiming 的中间值仍用 tap + 前缀重放，不必为此保留整条旧流水线。

无使用观测视图的存活（方言层，避免 backend 补丁）：

- `pyc.alias` / `pyc.delay_tap` 去掉无条件 `Pure`，改为 `MemoryEffectsOpInterface`：带外部可读身份时声明对观测资源的读/写副作用，DCE 会留下它们；普通功能 alias/tap 仍无副作用，无使用时照常删。
- `EliminateDeadState`：结果只被观测视图使用的状态视为活；不再把 `debug_keep` 理解成「即使没有任何读也留下一颗孤立 flop」（孤立且不可重构的身份视图本身就会把它读活）。

`FuseCombPass` 继续把 `hasStableStateName` / `shouldKeepStateOptimization` 当 barrier，避免名字被卷进 `pyc.comb` 后变成 Local、探针失效。

### 文件和接口改动清单

| 文件 | 改动目的 |
|---|---|
| `compiler/mlir/lib/Transforms/StripStateObservabilityPass.cpp` | 删除 |
| `compiler/mlir/CMakeLists.txt` | 移出该源文件 |
| `compiler/mlir/include/pyc/Transforms/Passes.h` | 删除 `createStripStateObservabilityPass`；三个 factory 去掉 `preserveObservability` |
| `compiler/mlir/include/pyc/Transforms/StateOptimization.h` | 增加 attachment / 物化视图 API；`isStateOptimizationCandidate` 等不再吃 preserve 开关 |
| `compiler/mlir/lib/Transforms/StateOptimization.cpp` | 实现收集与物化；pinned 不再禁优化 |
| `compiler/mlir/lib/Transforms/CombineDelayChainsPass.cpp` | 去掉 preserve 选项；合并/收链/共享后挂视图；structural 始终允许只读 fanout→tap |
| `compiler/mlir/lib/Transforms/RetimePipelinesPass.cpp` | 去掉 preserve；pipeline 为带身份的级补 tap/线；共同下沉后具名源 history 留下只读观测 |
| `compiler/mlir/lib/Transforms/PackStateLanesPass.cpp` | 去掉 preserve；所有外部身份都生成 slice alias 并拷属性 |
| `compiler/mlir/lib/Transforms/PackI1RegsPass.cpp` | 名字在 reg 上时也生成带属性的 alias；始终由 pycc 调用 |
| `compiler/mlir/lib/Transforms/EliminateDeadStatePass.cpp` | 观测视图视为使用；调整 `debug_keep` |
| `compiler/mlir/include/pyc/Dialect/PYC/PYCOps.td` 与对应 `*.cpp` | alias / delay_tap 的 effects，保证观测 sink 不被 DCE |
| `compiler/mlir/tools/pycc.cpp` | 删除 flag、strip 插入、preserve 传参和相关 JSON/stderr 字段 |
| `compiler/mlir/tools/pyc-opt.cpp` | 去掉 strip 的强制引用 |
| `compiler/mlir/test/state_delay_optimization_smoke.sh` 及对应 mlir/py | 删除 strip/preserve 用例；改为「优化后名字仍在」 |
| `compiler/mlir/test/state_observability_performance.mlir` | 改为观测-only 状态保留视图、不再整函数删光 |
| `compiler/mlir/test/state_retime_pipeline.mlir` | 去掉 PRESERVE 前缀 |
| `compiler/mlir/test/delay_line_diagnostics_smoke.sh` | 去掉 `preserve_observability=` 摘要 |
| `docs/delay_line.md`、`docs/MLIR_PASS_PIPELINE.md` | 同步新契约 |

不改 C++ runtime 原语；`addReg` / `addRegSlice` / tap 的 `qNext` 路径沿用。

### 边界条件、错误处理与兼容性

- 功能合法性（clk/rst/en/init/next、fanout、第二状态消费者、组合锥白名单、retiming 预算）不放宽。
- 两个不同名字的等价状态合并后，两个名字都 alias 到 survivor，读到同一值。这是等价合并的自然结果。
- 观测-only 且独一无二的状态：留下物理对象 + 原名字。与另一颗功能状态等价的 `debug_keep` / 具名副本可以并掉，两个名字都读 survivor。
- 共同下沉：功能 bits 可以变少；具名源占用的 bits 和 C++ 对象仍在，直到后续 packing 把观测 lane 打进宽存储（若控制兼容）。不把具名源的名字改挂到 `f` 的结果上。
- `pyc-opt` 单独跑 combine/retime/pack 时与 `pycc --emit=cpp` 同一套「优化 + 拉视图」，不再有 preserve 分叉。
- 统计：删除 `state_opt_preserve_observability`、`state_opt_observability_attrs_stripped`、`state_opt_observation_aliases_removed`。如需审计可另加 `state_opt_observation_views_materialized`（实施时按测试需要决定是否加）。
- 与 Decision 0003/0004/0018/0019 一致：稳定路径仍按 `pyc.name` 注册；只是存储可能变成 survivor / slice / tap。
- 与「语义在 dialect + MLIR pass」一致：重映射在 pass 里做完，emitter 继续只认已有的 name / pack lsb / tap。

## 与既有文档和约束的一致性检查

| 文档 | 结论 |
|---|---|
| `docs/updatePLAN.md` / `docs/rfcs/pyc4.0-decisions.md` | 不改 probe 路径规则；改的是 IR 里名字挂在哪个 op。需在 pass 内完成，不在 emitter 猜名字。 |
| `docs/delay_line.md` §6 / §13 | 与「默认剥身份、preserve 钉死」直接冲突，按本方案改写。 |
| `docs/MLIR_PASS_PIPELINE.md` | 删第 17/17b 行；pack-i1 条件改为始终（C++ 路径）。 |
| `docs/cycle-combine-drop-policy-flags-需求分析与实施规划-20260929.md` | 当时保留 `--state-opt-preserve-observability`，本次删除。 |
| `docs/cycle-combine-drop-analyze-passes-需求分析与实施规划-20260929.md` | 当时保留 strip；本次删除 strip，保留真正改写的 combine/retime/pack。 |
| `docs/cycle-combine-state-opt-cpp-only-需求分析与实施规划-20260929.md` | 不冲突。本方案只改 C++ 路径内部策略。 |
| `docs/IR_SPEC.md` `pyc.alias` | 仍是挂稳定调试名的 identity op；本次让带观测属性的 alias 对 DCE 不可见消失。 |

## 测试与验证计划

实施获得批准后改测试，不在批准前改。

1. 增量编译 `pycc` / `pyc-opt`（沿用仓库 `$pyc-build-v40` / 现有 delay-line toolchain 路径）。
2. `compiler/mlir/test/state_delay_optimization_smoke.sh`：去掉 strip 与 `--state-opt-preserve-observability`；pack probe 默认路径也必须留下 `lane0_state` / `lane1_state`；C++ 仍出现 `addRegSlice`。
3. 新增或改写 mlir FileCheck：等价合并后两个名字都还在且指向同一 survivor；收链后中间名在 `delay_tap` 上；pipeline retiming 后中间名在 tap/重放线上；共同下沉后功能结果已收窄，具名源 history 与原名仍在且值不变。
4. `check_state_delay_tap_models.py` / `check_state_retime_models.py`：功能对照不变。
5. `delay_line_diagnostics_smoke.sh`：更新 stderr 摘要，不再提 preserve。
6. 用 `xz_value_model_smoke` 同类输入跑 `pycc --emit=cpp --probe-manifest`，确认字段 `q` 仍在。
7. 性能：不在本次引入新的大规模 bench。用现有 delay-line / retime smoke 的 stats 确认 merge/chain/retime 次数不因「留下名字」回到 preserve 时代的 0。基线是当前默认（strip 后优化）的 stats；通过阈值是「同类夹具仍发生合并/收链/retiming」，而不是追微秒。

无法在规划阶段给出新的 ns/cycle 数：本机是否已编好 toolchain 不影响方案对错，实施后用上述 smoke 的 JSON 字段对比。

## 实施步骤

1. 在 `StateOptimization` 落地 attachment 收集与三种视图物化，并加「观测 sink 不可 DCE」的方言 effects。
2. 改 combine / retime / pack / pack-i1 / dead-state，删除 preserve 分支。
3. 从 pycc / pyc-opt / CMake / Passes.h 删除 strip 与 flag。
4. 更新测试与 `docs/delay_line.md`、`docs/MLIR_PASS_PIPELINE.md`。
5. 编译并跑上一节命令，把实际 stats 与任何偏差写回本文「实施结果」。

## 待确认事项

无。三项已拍板，实施按下面理解，不再分叉。

共同 delay 下沉选 B：功能路径改成 `D^N(f(...))`，带外部可读身份的源 history 留下只读观测，原名仍读到下沉前同一周期的 `x`/`y`，不把名字改挂到 `f` 的结果上。实现优先留下原对象，不 clone 第二份相同 history。没有外部身份的源仍可删除。

`debug_keep` 是只读观测，与 name/probe 相同：不钉死物理 flop。等价状态可以合并，多个观测名挂到 survivor；独一无二、不可重构的观测状态留下物理对象。

`cycle_balance` 的 `_v5_bal_*` 不拉外部视图，收链后可以随原对象消失，也不进入 probe 清单。

## 实施结果

已改完，尚未提交。叠在工作区已有的「状态优化只跑 `--emit=cpp`」改动上。

### 契约

- 删除 `pyc-strip-state-observability` 与 `--state-opt-preserve-observability`。
- `pycc --help` / `pyc-opt --help` 不再列出这两项；`pycc … --state-opt-preserve-observability` 按未知选项失败（rc=1）。
- `--emit=cpp` 仍跑 merge / 收链 / retiming / packing；身份不再挡住候选。
- `--emit=verilog` / `--emit=none` 仍不跑这组改写（`state_opt_policy=off`）。
- 观测-only 状态由 `pyc-eliminate-dead-state` 留下；带观测身份的 `pyc.alias` / `pyc.delay_tap` 声明 Write effect，canonicalize 不再折掉。

### 验证

```text
cmake --build .pycircuit_out/toolchain/build --target pycc pyc-opt
PYCC=$PWD/.pycircuit_out/toolchain/build/bin/pycc \
PYC_OPT=$PWD/.pycircuit_out/toolchain/build/bin/pyc-opt \
  bash compiler/mlir/test/state_delay_optimization_smoke.sh
# PASS；delay-tap checksum=1269766442396689593；retime checksum=4607132513909987476

PYCC=… PYC_OPT=… bash compiler/mlir/test/delay_line_diagnostics_smoke.sh
# PASS
```

默认 C++ 夹具 stats（与 strip 后优化同一量级，没有回到 preserve=0）：

| 字段 | 值 |
|---|---|
| `state_opt_policy` / `state_retime_policy` | `structural` / `pipeline` |
| `state_opt_regs_merged` / `state_opt_reg_bits_removed` | 1 / 8 |
| `reg_count` / `reg_bits` | 1 / 8 |
| Verilog 同输入 | `off`，`reg_count=2`，`reg_bits=16` |
| diagnostics merge / delay_chains / delay_lines | 2 / 1 / 1 |
| pack probe | `lane0_state` / `lane1_state` 为 state；`addRegSlice<8,16>` ≥ 4 |

`xz_value_model_smoke`：`python3 -m pycircuit.cli emit` 后 `pycc --emit=cpp --probe-manifest`。`q` 仍是 `kind=state`，C++ 有 `addReg<8>(…, "q")`。未跑完整 semantic/Verilator 门。

FileCheck 样例：

- `@merge_named_pair`：一颗 survivor，`copy_a` / `copy_b` 两个 alias。
- `@keep_named_intermediate`：`delay_line` + `delay_tap` + `architectural_tap` alias。
- `@preserve_named`：仍 retime（`retime_regions_rewritten=1`），`stage0` 挂在 tap 后的 alias。
- `@named_common_sink`：功能下沉（`retime_common_delay_sinks=1`，`state_bits_removed=15`），`lhs_hist` / `rhs_hist` 留在原 source reg 上。

### 实施中的修正

- TableGen 接口名是 `MemoryEffectsOpInterface`，不是 `MemoryEffectOpInterface`。
- `@named_common_sink` 的 FileCheck 必须先匹配源名字、再匹配 sink 属性：源 history 按原顺序留在 IR 里。
- 未加 `state_opt_observation_views_materialized`（方案允许按测试需要决定）。
- `StateObservabilityAnalysis` 仍在，改写路径已不再用它挡候选。
- smoke 默认路径仍是不存在的 `build-delay-line`；本次用 `.pycircuit_out/toolchain/build`。
