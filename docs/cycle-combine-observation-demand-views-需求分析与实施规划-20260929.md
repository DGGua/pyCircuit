# 按观测需求保留 C++ 视图需求分析与实施规划

## 背景与目标

`signal()` / `cycle()` 几乎总会通过 `Circuit.out()` 给寄存器的 `q` 挂上 `pyc.alias { pyc.name = "..." }`。这个字符串有两种完全不同的用途，当前实现把它们当成同一件事：

1. **作者身份**：设计里给这根状态起的稳定名字，方便人读 IR、对账、以及 `@probe` 在目录里按路径查找。
2. **运行时读取需求**：仿真时真的有人要按这个名字拿值——`@probe` 的 `dut.read`、VCD/trace 选中的内部场、`ProbeRegistry::findByPath`、或 IR 上的 `pyc.debug_keep` / `pyc.probe*` / `pyc.trace*` / `pyc.observable`。

上一轮「保留身份并重映射视图」之后，合并 / 收链 / retiming / packing 仍会做，但**每一个非 `cycle_balance` 的 `pyc.name` 都会**：

- 在优化后物化一根只读观测 alias（packing 时往往是 `名字 = extract(packed, lsb)`）；
- 因 `AliasOp` 带观测身份而标 `Write`，逃过 DCE；
- 被 `CppPlacementPass` 钉在 DUT struct 上（`pinToStruct` 看见 `pyc.name` 就钉死）；
- 被 `CppEmitter` 收进 `namedProbes`，每拍赋值，并 `addReg` / `addRegSlice`；
- 被 `FuseCombPass` 当成融合屏障（`hasStableStateName`）。

Davinci 一类设计里几百个 `{prefix}_ready_state` 都有用户 `name=`，但 PTO 功能仿真只走端口的 `SetLane` / `GetLane`，**从不按这些内部名读**。观测拷贝每拍仍在写，这是 keep-identity 后性能测试回退的主因，不是 packing 的功能 `extract`。

功能 `extract` 不能按需：packed lane 的后续组合逻辑和 next 每拍都要用。能按需的只有**观测视图**（多出来的 alias、struct 钉扎、`addReg`、fuse 屏障）。

目标：

1. **编译期分辨**哪些 `pyc.name` 有运行时读取需求，哪些只是作者身份。
2. 状态优化阶段仍然按身份重映射，不复活「优化前整表剥名字」。
3. 优化完成之后、C++ placement / fuse / emit 之前，丢掉没有读取需求的观测视图，使无人读的名字不再进入热路径。
4. 有读取需求的名字行为与现在 keep-identity 相同：同一周期、同一位宽，`dut.read` / VCD / `addRegSlice` 仍可用。

不在本次做「`eval` 里 `if (这拍有人 read) extract`」。那种热路径分支对便宜的移位/赋值没有收益，VCD 打开时等于每拍都有需求。

## 项目现状与执行流程定位

端到端 `python3 -m pycircuit.cli build` 已经是两段式，读取需求在发 C++ 之前就能齐：

```text
前端 .pyc
    │
    ▼
pycc --emit=none --probe-manifest          ← 不跑 C++ 状态优化
    写出 device/probe_catalog.json           目录含全部 pyc.name（除 cycle_balance）
    │
    ▼
CLI 解析 @probe(target=...)                 ProbeView.read("旧名字")
    写出 probe_plan.json                     aliases: canonical_path → source_path
    若有 --trace-config：trace_codegen_plan   每模块被选中的内部 field
    │
    ▼
pycc --emit=cpp --probe-plan --trace-codegen-plan
    combine / retime / pack                  现在：凡有 pyc.name 就物化视图
    fuse-comb                                现在：凡有 pyc.name 就不可融合
    cpp-placement                            现在：凡有 pyc.name 就钉 struct
    CppEmitter                               现在：凡有 pyc.name 就 addReg + 每拍赋值
```

`@probe` 定义在 `compiler/frontend/pycircuit/design.py`，解析在 `compiler/frontend/pycircuit/probe.py`。它不是硬件，只是在目录上选叶子。例如 `xz_value_model_smoke.py` 里 `dut.read("q")` 发生在**编译期解析**，结果写进 `probe_plan.json`，不是仿真热路径上的 Python 查询。

`IGeneratedBackend`（Davinci 适配器）只按**端口名**读写。内部 `{prefix}_ready_state` 不在端口表里，功能仿真没有读取需求。

直接调用 `pycc file.pyc --emit=cpp`（不经 CLI、不传 `--probe-plan`）时，工具**看不到** `@probe` / trace 选择。这时只能看见 IR 属性。仓库里不少 smoke（`state_delay_optimization_smoke.sh`、`state_pack_probe_runtime.cpp`）走这条路，并用 `findByPath("dut:lane0_state")` 断言内部名仍在 C++ registry 里。

`pyc-opt` 的 FileCheck 夹具检查的是优化后 MLIR 上是否还挂着 `pyc.name`，不经过 C++ emit。它们需要身份在 combine/retime/pack 之后仍可见。

## 相关现有实现说明

### 名字怎么进 IR

`compiler/frontend/pycircuit/v5.py` 的 `signal()` / `cycle()`：用户没写 `name=` 时生成 `_v5_reg_N`。Davinci 源码里 657 处都带了用户 `name=`，没有 `_v5_reg_`。

`compiler/frontend/pycircuit/hw.py` 的 `Circuit.out()`：先建匿名 `pyc.reg`，再 `alias(q, name=fullname)`。`pyc.name` 通常在 alias 上，不在 `pyc.reg` 上。后面的功能逻辑 `.read()` 用的是这根 alias 的 SSA，不是运行时查字符串。

周期对齐插入的寄存器带 `pyc.generated = "cycle_balance"` 和 `_v5_bal_*`。`pycc` 写 probe catalog 和 `CppEmitter` 收集 `namedProbes` 时已经跳过它们。

### 身份如何迫使 C++ 付费

`compiler/mlir/lib/Transforms/StateOptimization.cpp`：

- `hasExternalObservationIdentity`：非 `cycle_balance` 的 `pyc.name`，或 `pyc.debug_keep` / `pyc.observable` / `pyc.probe*` / `pyc.trace*`。
- `materializeObservationAlias` / `remapStateOpIdentity`：优化换掉物理对象后，把上述属性挂到新的只读 alias 上。

`compiler/mlir/lib/Dialect/PYC/PYCOps.cpp` 的 `AliasOp::getEffects` / `fold`：带上述身份的 alias 视为有 `Write` 且不可折掉，避免观测-only 视图被 DCE。这是**活性**，不是真存储。

`compiler/mlir/lib/Transforms/FuseCombPass.cpp`：`hasStableStateName` 或 `shouldKeepStateOptimization` 的 op 不能融进更大的 comb。

`compiler/mlir/lib/Transforms/CppPlacementPass.cpp` 的 `pinToStruct`：定义端有 `pyc.name` 就钉 DUT struct，保证 `ProbeRegistry` 指针稳定。`--trace-codegen-plan` 已经能把「被 trace 选中的内部 comb 局部」钉住；**具名状态目前不看这份 plan，有名字就钉。**

`compiler/mlir/lib/Emit/CppEmitter.cpp`：遍历带 `pyc.name` 的单结果 op，生成 `pyc_register_probes` 的 `addReg` / `addRegSlice`，以及 `eval` 里对 alias 的赋值。`--probe-plan` 只额外 `addAlias`（把 `@probe` 的 `probe.xxx` 路径指到已注册的 source），**不用来过滤 source 自己要不要注册。**

### 功能 extract 和观测 extract

`PackStateLanesPass` 对每个 lane 都会建 `pyc.extract`（功能替换），若原 lane 有外部身份再 `materializeObservationAlias`。功能 extract 必须留；多出来的那根 `名字 = extract` 只服务观测。

### 和已删除的 strip 的差别

已删除的 `pyc-strip-state-observability` 在**优化前**剥掉全部观测属性，导致合并/收链曾经看不到名字、也曾经靠剥名字才跑得动。当前契约是优化中保留身份并重映射。本方案**不把 strip 放回优化前**，只在优化**之后**按需求表丢掉无人读的视图。

## 需求与验收标准

1. 编译期必须能回答：这个 `pyc.name` 是否属于「运行时观测需求集合」。答案来自下面「需求集合」定义，不允许在 `eval` / `comb` 里按拍询问。
2. `pyc.name` 单独存在（没有出现在需求集合里）时：C++ 热路径不得再为它付观测税（不钉只因名字、不 `addReg`、不保留观测-only alias 的每拍赋值、不因这个名字阻止 fuse-comb）。功能 SSA 与 packing 的功能 `extract` 不变。
3. 需求集合内的名字：优化后仍有只读视图；`probe_plan` 的 `source_path`、`findByPath`、VCD enable、`addReg` / `addRegSlice` 行为与现在 keep-identity 一致。
4. `--emit=none` 的 `probe_catalog.json` 仍枚举全部非 `cycle_balance` 的 `pyc.name`，供 `@probe` 解析。目录可以大于 C++ `ProbeRegistry`。
5. `cycle_balance` / `_v5_bal_*` 仍不是外部探针，不进入需求集合。
6. 端口始终按现有规则注册和采样，不依赖内部 `pyc.name` 是否在需求集合中。
7. `--emit=verilog` / `--emit=none` 仍不跑 C++ 状态优化；本方案只作用于 `--emit=cpp` 的后半段。
8. `pyc-opt` 默认不丢名字，现有「优化后 MLIR 仍有 `pyc.name`」的 FileCheck 继续成立。
9. `xz_value_model_smoke` 这种带 `@probe(dut.read("q"))` 的夹具，经 `cli build` 后 C++ 仍能解析 `q`。
10. 提供显式逃生口，把「所有非 `cycle_balance` 的 `pyc.name`」重新当成需求（恢复当前 C++ 行为），供 raw `pycc` 夹具和临时 dump 全部内部状态。
11. 不引入运行时「有人 read 才 extract」的分支。

## 方案设计

### 模块边界、输入与输出

新增一层概念，与「外部可读身份」分开：

| 术语 | 含义 | 用在哪 |
|---|---|---|
| **作者身份** | 非 `cycle_balance` 的 `pyc.name`，以及 `debug_keep` / `observable` / `probe*` / `trace*` | combine / retime / pack 重映射视图；`--emit=none` catalog |
| **运行时观测需求** | 见下一小节的集合 | 优化后的需求降级 pass；fuse-comb 屏障；placement 钉扎；emitter 注册 |

**运行时观测需求集合**（对每个 `func` / 模块分别看 `field_path`，即 `pyc.name` 字符串）是下列并集：

1. IR 上已有 `pyc.debug_keep`、`pyc.observable`、任意 `pyc.probe*`、任意 `pyc.trace*` 的 op 所带的 `pyc.name`（若有）。没有 `pyc.name` 但有这些属性的，整棵 op 仍视为有需求（与现在 `shouldKeepStateOptimization` 一致）。
2. `--probe-plan` 里每条 alias 的 `source_path` 所对应的 `field_path`（`instance:field` 的冒号右侧；顶层常见 `dut:q` → `q`）。
3. `--trace-codegen-plan` 里该模块列出的内部 field。
4. 逃生口 `--observe-named=all`：全部非 `cycle_balance` 的 `pyc.name`（当前行为）。不提供 `--observe-fields`（已拍板只要开关）。点名走 `@probe` / `--trace-config` / IR 属性。

**不纳入需求、也分辨不了的：**

- 仿真开始后才拼出来的 `findByPath(动态字符串)`；
- 没有 `@probe`、没有 trace 选择、没有 IR 观测属性的裸 `name=`。

这类访问在 demand 模式下会找不到 registry 条目。要读就必须在编译期把它放进需求集合。这与 Decision 0091 / 0096（选择性插探针，未选中的探针不应有运行时开销）一致。

**不扫测试台源码。** 不解析 C++/Python 文本里的 `findByPath("...")` / `dut.read("...")`。`dut.read` 在 `@probe` 里已经是编译期解析；C++ 侧应改走 `@probe`、`--trace-config`、IR 属性或 `--observe-named=all`。

### 上下游关系与数据/控制流

C++ 路径改成：

```text
eliminate-wires
eliminate-dead-state
combine-delay-chains (merge / cascade)
retime-pipelines
combine-delay-chains (form delay-line / tap / share)
pack-state-lanes / pack-i1
        │
        │  身份仍在，视图已按 keep-identity 挂好
        ▼
pyc-apply-observation-demand          ← 仅 --emit=cpp，且 --observe-named≠all
        不在需求集合的 pyc.name 从观测属性里拿掉
        只靠名字活着的 alias 恢复可 DCE（不再 Write）
canonicalize / remove-dead-values     ← 删掉观测-only 且无人用的 alias
        ▼
fuse-comb                             ← 不再被无人读的名字挡住
cpp-placement                         ← 不再只因无人读的名字钉 struct
emitter / ProbeRegistry               ← 只注册还在的名字 + 端口 + probe-plan alias
```

`--emit=none` 写 catalog 的时机不变，仍在状态优化之前，因此 `@probe` 能看到优化前就存在的全部作者名。优化后名字字符串不变（重映射不改 `field_path`），所以 `probe_plan` 的 `source_path` 仍能对上降级后留下的视图。

直接 `pycc --emit=cpp` 且用户选择 `--observe-named=all`（或按待确认事项选定的默认）时，**不插入** `pyc-apply-observation-demand`，C++ 行为与现在相同。

`pyc-opt` 默认不插入该 pass。需要时手动：

```text
pyc-opt --pyc-apply-observation-demand='demand-file=plan.json'
```

### 文件和接口改动清单

| 文件 | 改动目的 |
|---|---|
| `compiler/mlir/include/pyc/Transforms/StateOptimization.h` 与 `.cpp` | 增加「是否属于需求集合」查询；需求集合从 JSON/CLI 载入；与 `hasExternalObservationIdentity` 明确分工 |
| 新 pass：`compiler/mlir/lib/Transforms/ApplyObservationDemandPass.cpp` | 优化后按需求集合剥掉无人读的 `pyc.name`（不剥 `debug_keep` / `probe*` / `trace*` / `observable`）；统计剥了多少名字、多少观测-only alias 变为可 DCE |
| `compiler/mlir/include/pyc/Transforms/Passes.h` 与 CMake | 注册 `pyc-apply-observation-demand` |
| `compiler/mlir/lib/Dialect/PYC/PYCOps.cpp` | **不改** `carriesExternalObservationIdentity` 的属性集合。降级 pass 先拿掉无人读的 `pyc.name` 后，现有 `Write` / `fold` 规则自然让观测-only alias 可被 DCE |
| `compiler/mlir/lib/Transforms/FuseCombPass.cpp` | 无需改语义：降级后无人读的名字已不在，`hasStableStateName` 自动不再挡融合 |
| `compiler/mlir/lib/Transforms/CppPlacementPass.cpp` | 无需为「裸名字」再开例外：降级后 `pinToStruct` 只钉还在的名字；trace plan 路径保持 |
| `compiler/mlir/lib/Emit/CppEmitter.cpp` | 收集逻辑不变；registry 自然变小。`addAlias` 仍要求 source 已注册——source 必须在需求集合中（`@probe` 已保证） |
| `compiler/mlir/tools/pycc.cpp` | `--observe-named=all\|demand`（默认 demand）；在 pack-i1 之后、fuse-comb 之前插入需求 pass；把已有 `--probe-plan` / `--trace-codegen-plan` 喂给该 pass |
| `compiler/frontend/pycircuit/cli.py` | `cli build` 的 cpp 调用带上与默认一致的 `--observe-named`（见待确认）。`probe_plan` 已在传 |
| `docs/MLIR_PASS_PIPELINE.md`、`docs/delay_line.md` | 写清「身份重映射」与「需求降级」两段，避免再被理解成优化前 strip |
| `docs/cycle-combine-keep-identity-remap-views-需求分析与实施规划-20260929.md` | 实施后回写：C++ 热路径不再为无需求名字保活视图 |
| 测试 | 见「测试与验证计划」 |

不改 `IGeneratedBackend`、不改 Davinci `WorkSelf`、不改 `ProbeRegistry` 查找算法。不恢复 `pyc-strip-state-observability`。

### 边界条件、错误处理与兼容性

- `probe_plan` 引用了 catalog 里有、但 IR 优化后丢失且需求 pass 也找不到的 source：视为错误，`pycc` 失败并指出 `source_path`。正常 keep-identity 重映射不应丢字符串；这是防回归。
- 同一 `field_path` 出现多次：需求判断按字符串，去重即可。
- 只有 `pyc.name`、SSA 仍被功能逻辑使用：剥名字后 alias 可能被 `fold` 成输入，或留下无名 alias；功能连接不变，只是不再钉/注册。
- 只有 `pyc.name`、没有任何 SSA 使用者：剥名字后 DCE 删掉观测 alias；对应物理状态若只靠这根 alias 保活，`pyc-eliminate-dead-state` 可删——这是期望收益，与「没人读」一致。
- `debug_keep` 即使没有列入 plan，也始终是需求（人明确要求留着看）。
- `--probe-plan` 文件缺失或 JSON 坏：按现有 emitter 行为报错，不静默当成「无需求」。
- 空的 `probe_plan`（CLI 在没有 `@probe` 时也会写一份 `aliases: []`）：demand 模式下内部裸名字全部降级。这是 Davinci / 性能测试的主收益路径。
- Verilog 网表不跑本 pass，名字是否出现在 Verilog 注释/调试与本次无关。

## 与既有文档和约束的一致性检查

| 文档 / 决策 | 检查结论 |
|---|---|
| `docs/rfcs/pyc4.0-decisions.md` Decision 0004 / 0023 | 仍用中央 registry 和 `instance:field` 路径；只是注册子集变小。 |
| Decision 0091 / 0096 / 0095 | 选择性插探针；未选中的探针不应有运行时开销。当前「有 `name=` 就注册」与这两条冲突；本方案按决策收紧。 |
| Decision 0097 / 0100 | manifest / catalog 仍在 lowering 生成。`--emit=none` catalog 保持全集；C++ registry 与「实际 emit 的探针」对齐。 |
| Decision 0145 / 既有 `--trace-codegen-plan` | 被选中的内部场进入需求集合，placement 继续为它们钉存储。 |
| `docs/updatePLAN.md` | 语义在 dialect + pass（新的 demand pass + 沿用 alias 活性规则），不是 C++ runtime 里偷偷少算。 |
| `docs/cycle-combine-keep-identity-remap-views-需求分析与实施规划-20260929.md` | **有意收窄**：优化中仍保留并重映射全部身份；C++ 热路径不再为「无读取需求的名字」保活视图。不恢复优化前 strip。验收第 10 条「无使用观测视图不得被 DCE」改为「无使用且**有需求**的视图不得被 DCE」。 |
| `docs/delay_line.md` §6 / §13 | 需在实施后改成「身份重映射 + 需求降级」，避免读者以为每个 `pyc.name` 都会 `addReg`。 |
| `docs/MLIR_PASS_PIPELINE.md` | 需在 pack 与 fuse-comb 之间增加 demand pass。 |
| 已删除的 strip / `--state-opt-preserve-observability` | 不恢复。逃生口叫 `--observe-named=all`，作用在优化之后，不钉死合并。 |

无无法规避的硬冲突。与 keep-identity 文档的差异是契约收窄，实施前必须在待确认事项里拍板默认策略。

## 测试与验证计划

功能 / 回归：

1. 新 MLIR 夹具（`pyc-opt` 手动跑 demand pass）：两个具名 `pyc.alias`，只有一个名字出现在假的 probe-plan 里。跑 combine/pack 后再跑 demand + canonicalize。有需求的名字和 `addReg` 源还在；无需求的观测-only alias 被删；功能 `extract` / 端口仍在。
2. `xz_value_model_smoke`：`cli build`（有 `@probe` 读 `q`）后 C++ 含 `addReg`/`findByPath` 能对上 `q`。
3. 现有 `state_delay_optimization.mlir` 等 **pyc-opt FileCheck**：默认管线不跑 demand pass，名字仍在。
4. `state_pack_probe_runtime.cpp` / `state_delay_optimization_smoke.sh`：这类 **raw `pycc --emit=cpp` 且按内部名 `findByPath`** 的夹具，必须显式 `--observe-named=all`。禁止依赖「默认再把全部名字当探针」。
5. 无 `@probe` 的最小模块经 `cli build`：生成 C++ 的 `pyc_register_probes` 只有端口（以及 plan 里的 alias，此时为空），没有 `{prefix}_ready_state` 一类内部 `addReg`。
6. `--observe-named=all` 回归：同一模块内部 `addReg` 数量回到当前 keep-identity 水平。
7. `probe_plan` 指向不存在的 `source_path`：`pycc` 非 0 退出。

性能（改动前后各跑，同一输入，至少 3 次取中位数）：

```text
# 基线：当前分支 keep-identity（全部名字留视图）
# 对照：本方案 demand 默认（无 @probe 的 IQ / TbIssq / 与性能测试相同的 pycc 命令）

# 生成物计数（不跑仿真）
rg -c 'addReg|addRegSlice' generated.hpp
rg -c 'pyc::cpp::Wire' generated.hpp

# 若沿用现有「pycircuit 性能测试」作业：同一 design、同一 cycle 数、同一机器
# 通过阈值：demand 模式相对当前 keep-identity，内部 addReg 数回到 dc0599fd（strip 时代）同量级；
# 墙钟不得慢于当前 keep-identity。目标是收回 199eb03e 相对 dc0599fd 的大部分回退。
```

测量脚本复用现有性能测试入口（不新造 bench 框架）。若本机没有那套 design，用仓库内 `designs/IssueQueue` 或 TbIssq 的 `pycc --emit=cpp` 生成物计数作为门禁，墙钟作为人工对照写回本文「实施结果」。

## 实施步骤

1. 落地需求集合载入（probe-plan + trace-codegen-plan + IR 属性）和单测。
2. 实现 `pyc-apply-observation-demand`：只剥无需求的 `pyc.name`，不剥 `debug_keep` / `probe*` / `trace*` / `observable`；打 stats。
3. 在 `pycc --emit=cpp` 于 pack 之后、fuse-comb 之前插入；接 CLI 旗标。
4. 更新 `cli.py` 传参（与待确认的默认一致）。
5. 改 raw pycc 的 pack-probe / observability smoke，改为显式 `--observe-named=all` 或列出字段。
6. 更新 `MLIR_PASS_PIPELINE.md`、`delay_line.md`、keep-identity 规划的契约段落。
7. 跑 `$pyc-build-v40` 相关 smoke + 上述新夹具；记录生成物计数和（若可得）性能测试中位数。
8. 把命令、数字、与计划的偏差写回本文「实施结果」。

## 待确认事项

已拍板（2026-09-29）：待确认 1 选 **B**（`pycc --emit=cpp` 默认 `--observe-named=demand`；无 plan 只留 IR 观测属性和端口）。待确认 2 选 **A**（只有 `--observe-named=all|demand`，不新增 `--observe-fields`）。点名字段走 `@probe` / `--trace-config` / IR 属性。

下列原文保留作决策记录。

### 待确认 1：`pycc --emit=cpp` 在没有人声明读取时，默认丢掉哪些名字

**为何现在必须决定：**
当前每个非 `cycle_balance` 的 `pyc.name` 都会变成 C++ struct 成员、每拍赋值和 `ProbeRegistry` 条目。本方案要在优化之后按「运行时观测需求集合」丢掉无人读的名字。`python3 -m pycircuit.cli build` 总会先写 `probe_catalog.json`、解析 `@probe`、再调用 `pycc --emit=cpp --probe-plan`。但仓库和 Davinci 也有人直接跑 `pycc foo.pyc --emit=cpp`，这时没有 plan 文件。默认选错，要么性能测试/Davinci 继续慢，要么一批 raw pycc 夹具（例如按 `dut:lane0_state` 做 `findByPath` 的 pack probe runtime）会突然找不到内部名。

**相关背景：**
「作者身份」是 IR 上的 `pyc.name`，给设计起名用。`@probe` 里的 `dut.read("q")` 发生在编译期，结果进 `probe_plan.json`。VCD/trace 的内部场进 `trace_codegen_plan.json`。IR 上的 `pyc.debug_keep` / `pyc.observable` / `pyc.probe*` / `pyc.trace*` 也算需求。端口始终注册。仿真里事后拼字符串去 `findByPath` 的，编译期看不见。已删除的 `pyc-strip-state-observability` 不会回来；combine/retime/pack 仍然先带着全部名字做完重映射。`pyc-opt` 的 FileCheck 不走这条 C++ 默认，不受本项影响。

**选项与结果：**

- **选项 A：无 `--probe-plan` 时当作「全部名字都有需求」（`--observe-named=all`）**
  - 将做什么：只有 `cli build` 这种传了 `--probe-plan` 的路径按需求降级；空 `aliases` 会丢掉所有裸内部名。
  - 结果：现有 raw `pycc --emit=cpp` 夹具不用改。Davinci / 性能测试如果也是 raw pycc 且不传 plan，**没有加速**。
  - 风险：主收益路径可能根本没开。后续还要改 Davinci/perf 的编译命令。

- **选项 B：`pycc --emit=cpp` 默认 `--observe-named=demand`；无 plan 就只保留 IR 观测属性和端口**
  - 将做什么：裸 `name=` 在 C++ 里不再注册、不再只因名字钉 struct。逃生口 `--observe-named=all` 恢复现在。
  - 结果：Davinci / 性能测试即使 raw pycc 也会加速。`state_pack_probe_runtime.cpp` 等必须改成 `--observe-named=all` 或列出 `lane0_state`。
  - 风险：漏改的 raw pycc 测试会失败；有人靠「有名字就能 dump」的临时 C++ 会找不到路径，需要加旗标或 `@probe`。

- **选项 C：默认 demand，但 `cli build` 以外的 raw pycc 再默认 all**
  - 将做什么：靠「是否出现 `--probe-plan`」切换：有 plan（含空 aliases）→ demand；完全没这个旗标 → all。
  - 结果：`cli build` 无 `@probe` 的设计（多数性能作业若走 CLI）会加速；纯 raw pycc smoke 不用改。
  - 风险：Davinci 若走 raw pycc 且不传 plan，仍然不加速。行为对「多了一个空 plan 文件」敏感，需要在帮助文本里写清楚。

**推荐：**
选项 B。和 Decision 0091/0096（默认不要把所有信号当探针）一致；主收益不依赖调用方记不记得传 plan。夹具改动是一次性的，而且本来就在测「内部名必须能读」，理应显式声明需求。

### 待确认 2：逃生口的名字与是否允许按字段点名

**为何现在必须决定：**
demand 默认落地后，调试和旧夹具需要一条不写 `@probe` 也能把内部名留在 C++ 里的路。接口不先定，实施时会同时出现 `--observe-named`、`--observe-fields`、复用 `--trace-codegen-plan` 三套说法。

**相关背景：**
`@probe` 适合长期、结构化的读取。`--trace-config` 已经能选出要打波形的内部场，并生成 `trace_codegen_plan.json`。有人只想对 raw `pycc` 说「这三根线留着」或「这次把全部名字当探针」。不在 C++ `eval` 里做按拍开关。

**选项与结果：**

- **选项 A：只要 `--observe-named=all|demand`，字段级选择只走 `@probe` / `--trace-config`**
  - 将做什么：不新增 `--observe-fields`。
  - 结果：接口少。pack-probe 一类夹具只能 `all`（内部名全留，失去「只留两根」的精度）或改写成 `@probe`。
  - 风险：小夹具为两根名字被迫 `all`，C++ 又变胖，但夹具通常很小，可接受。

- **选项 B：`--observe-named=all|demand` 加上 `--observe-fields=field1,field2`（精确匹配 `pyc.name`）**
  - 将做什么：点名的字段并进需求集合。
  - 结果：raw pycc 夹具可以只留 `lane0_state,lane1_state`，更接近真实 demand。
  - 风险：多一个旗标；不做 glob，避免和 trace 过滤语言重复。

**推荐：**
选项 B。实现只是字符串集合并上，夹具和临时 dump 都用得上。

## 实施结果

已按拍板 B+A 改完，未提交。未混入工作区里其它未跟踪文档。

### 契约

- `pycc --emit=cpp` 默认 `--observe-named=demand`：优化并重映射身份之后，剥掉不在 `@probe` source / `--trace-codegen-plan` / IR 观测属性里的 `pyc.name`。
- `--observe-named=all` 跳过该 pass，C++ 行为回到 keep-identity（每个非 cycle-balance 名字都注册）。
- 不提供 `--observe-fields`。`cli build` 显式传 `--observe-named=demand` 和已有 `--probe-plan`。
- `pyc-opt` 默认不跑该 pass；可手动 `--pyc-apply-observation-demand`。
- `--emit=none` catalog 仍枚举全部作者名。

### 验证

```text
cmake --build .pycircuit_out/toolchain/build --target pycc pyc-opt
PYCC=$PWD/.pycircuit_out/toolchain/build/bin/pycc \
PYC_OPT=$PWD/.pycircuit_out/toolchain/build/bin/pyc-opt \
  bash compiler/mlir/test/state_delay_optimization_smoke.sh
# PASS（含 observation_demand FileCheck、demand 下无 lane0_state、all 下 addRegSlice≥4）

PYCC=… PYC_OPT=… bash compiler/mlir/test/delay_line_diagnostics_smoke.sh
# PASS

pycc --observe-named=maybe → unknown --observe-named（rc≠0）
pyc-opt … probe-plan=/no/such/… → cannot read probe plan（rc≠0）
```

`cpp_member_placement_smoke.sh` 因本机构建目录缺少 installed runtime headers 未跑，与本次改动无关。

未跑完整「pycircuit 性能测试」墙钟；门禁用 pack-probe 生成物对照：demand 不再出现 `lane0_state`/`lane1_state`，`--observe-named=all` 仍 ≥4 个 `addRegSlice<8, 16>`。

### 实施中的修正

- 需求 pass 插在 `pyc-pack-i1-regs` 之后、`pyc-fuse-comb` 之前，避免 i1 pack 再挂回已剥掉的名字。
- probe-plan 按所有 `source_path` 的 `field_path` 收集，不按 `top_symbol` 过滤，避免 `--cpp-split=module` 编译子模块时丢掉被 `@probe` 选中的字段。
