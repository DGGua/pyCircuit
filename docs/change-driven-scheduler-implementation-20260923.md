# Change-Driven Scheduler 实现说明

日期：2026-09-23
分支：`feat/change-driven-scheduler`
基线提交：`7d84e293`；本文档同时描述其后的 dirty-only hard break
对比基线：`upstream/main` @ `27b226cd`
状态：核心机制已实现；本次复测 6 项通过、1 项因诊断文本顺序不一致失败；
正式端到端性能 A/B 尚未完成

## 1. 文档目的

本文集中说明 change-driven scheduler 的：

1. 实现背景和要解决的问题；
2. 已实现功能及其边界；
3. 编译期、生成代码和运行时的实现细节；
4. 实现后的完整执行流程；
5. 当前验证结果、已知问题和仍未完成的工作。

本文描述的是 `feat/change-driven-scheduler` 在 `7d84e293` 的实际代码，不把规划中
尚未完成的功能写成现状。

## 2. 背景

### 2.1 原有 C++ 仿真的重复求值

pyCircuit 的 C++ 仿真模型保留明确的阶段 API：

```text
comb()     = eval()
tick()     = tick_compute()
commit()   = tick_commit()
transfer() = tick_commit()
step()     = comb() → tick() → commit() → comb()
```

沿前和沿后各一次 `comb()` 分别服务于 TICK-OBS 和 XFER-OBS 语义，不能简单删除。
真正可优化的是每次 `comb()` 内部以及状态阶段中对“输入和依赖均未变化”的逻辑重复
求值。

原实现主要依赖：

- 每次执行全部 fused `pyc.comb`；
- instance 和 primitive 的输入缓存；
- 无法建立完整拓扑顺序时的 fixed-point/fallback 迭代；
- 状态提交后较粗粒度的 cache 失效。

这些机制能保证结果收敛，但低活动设计仍会反复扫描和执行大量没有语义变化的逻辑。

### 2.2 性能调查给出的方向

在实现 change-driven scheduler 前，对 Davinci qwen3-14b trace 做过路径计时：

- 顶层 `fallback_share` 中位数为 **99.9144%**；
- fallback 中重复 `eval_comb_pass()` 只占顶层 `eval()` 约 **0.6201%**；
- primitive/instance group 占顶层 `eval()` 约 **99.282%**；
- 顶层每次 fallback 固定迭代 4 次，shared 子实例平均迭代 8 次。

这组数字是旧调度路径的基线，不是 change-driven 实现后的加速比。它说明主要成本
不只是组合表达式计算，而是层次 instance、primitive、状态边界和父级扫描的重复
执行。因此实现需要覆盖完整的数据变化链路，而不能只给单个 `pyc.comb` 加缓存。

该基线测于 PyCircuit `282fee78` 和本地脏 Davinci `lys` @ `4e924f6c`，trace 为
`models_qwen3_14b_decode_fwd.pto.trace`，`clang++ 22.1.3 -O3`，并定义了
`PYC_DISABLE_INSTANCE_EVAL_CACHE`。因此它会放大 fallback 内的子实例重复求值，
不能直接代表本分支最终默认配置。

### 2.3 设计约束

实现遵守以下约束：

- gate-first：调度依赖必须先由 MLIR 统一分析和验证；
- C++ emitter 不重新猜测硬件依赖；
- 真实组合环仍由 IR gate 拒绝，不能依赖 runtime 迭代掩盖；
- 保持 `comb/tick/commit/transfer/step` API 和周期语义；
- 保持 C++ 与 Verilog 的硬件结果一致；
- 第一次求值必须完整初始化；
- 只有“当前二值 `Wire` 语义下值确实变化”才能唤醒下游；
- 无法由 change-driven 精确处理的边界采用保守 polling，而不是错误跳过。

## 3. 已实现功能

### 3.1 唯一的 dirty 更新策略

C++ emitter 固定使用 change-driven dirty 策略，不再提供运行时或编译期模式选择：

- `pycc` 和 Python build CLI 均没有 `--comb-update`；
- `CppEmitterOptions` 不再含 `CombUpdateMode`；
- 含 fused `pyc.comb` 的 C++ 模型生成 input snapshots 和 `DirtyBitset`；所有
  适用的结果发布固定使用语义比较和 metadata direct fanout；
- 旧的 `always`、`guarded` 和显式 `dirty` 命令均属于不兼容接口，传入旧参数会
  直接报 unknown/unrecognized option；
- Verilog emitter 不读取 C++ dirty 策略，硬件输出语义不受该 hard break 影响。

### 3.2 统一的编译期 change schedule

新增 schema：

```text
pyc.change_schedule.v1
```

每个函数具有 summary：

- `node_count`
- `edge_count`
- `rank_count`
- `schema`

每个可调度 op 的每个 result 具有：

- `pyc.change_schedule.node`
- `pyc.change_schedule.rank`
- `pyc.change_schedule.slot`
- `pyc.change_schedule.fanout`

调度节点覆盖 fused `pyc.comb`、`pyc.instance`、register、FIFO、memory、async FIFO
和 CDC primitive 的可见结果。

### 3.3 固定容量 dirty set

运行时新增 `pyc::cpp::DirtyBitset<Capacity>`：

- storage 全部内联；
- `mark()` 合并重复唤醒；
- `take()` 取走指定节点；
- `takeNext()` 按最小拓扑位置取下一个 dirty 节点；
- `test()`、`count()`、`clear()` 支持生成代码和测试；
- 不进行动态内存分配。

同时实现了 `VersionClock`、`VersionStamp` 和 `publishIfChanged()`，为语义变化发布、
版本回绕和显式 rebase 提供基础能力。当前 emitter 的主要 dirty comb 路径使用
`DirtyBitset` 和值比较；版本工具已经由 runtime unit test 覆盖。

需要注意：runtime 提供了 `takeNext()`，但当前 emitter 没有用它实现全图事件队列。
生成代码仍按既有 full-topology 或 fallback 控制流访问节点；dirty bit 主要通过
指定 `take(combIndex)` 跳过本地 fused comb 主体。

### 3.4 fused comb 的语义变化传播

dirty 模式下，每个 fused `pyc.comb`：

1. 首次执行前处于 dirty 状态；
2. 对来自 schedule node 的输入不再逐项 polling；
3. 对顶层输入等非 schedule boundary 继续保守 snapshot/polling；
4. 只有输出值与上次发布值不同时才写回；
5. 根据 MLIR metadata 中的直接 fanout 标记下游 comb dirty；
6. 重复 fanout 由 bitset 自动合并。

因此“producer 被执行”不等于“consumer 被唤醒”；只有 producer 的公开结果发生
语义变化才传播。

### 3.5 层次 instance

instance 路径实现了：

- 默认开启的子模块输入缓存；
- 小输入使用逐端口 value/version cache；
- 中大型输入使用 packed-word cache；
- nested vector 的 cache word 数按所有 lane 精确计算；
- 输入不变时跳过 child `eval()`；
- stateful child 在 commit 后失效，纯组合 child 保留 cache；
- child 每个输出独立比较；
- 只有发生变化的 child output 才唤醒对应本地 comb fanout；
- fallback 收敛使用 child 的语义输出变化，不只使用 input-cache miss。

可用编译宏关闭 cache 做参考对照：

```text
PYC_DISABLE_INSTANCE_EVAL_CACHE
PYC_DISABLE_VERSIONED_INPUT_CACHE
```

当前 IR 尚未硬化 callee output-to-input 的精确依赖摘要，因此 child 是否需要
重新进入 `eval()` 仍按输入变化保守判断；输出发布和本地 fanout 已是逐输出语义变化。

### 3.6 primitive 和状态边界

FIFO、byte memory、sync memory、dual-port sync memory、async FIFO、CDC 等
primitive 已接入 change reporting：

- `eval()` 前后 snapshot 可见输出；
- 只有变化的输出唤醒其直接 comb consumers；
- `tick_commit()` 返回 bool 或 bitmask；
- FIFO/memory 等 bitmask 区分具体哪个公开输出发生变化；
- commit 直接按 metadata fanout 唤醒 consumer，不再只依赖下一次 comb polling。

register 的 `tick_commit()` 也返回 Q 是否变化；Q 未变化时不会唤醒下游。

### 3.7 状态输入 dirty guard

dirty 模式为 instance 和本地状态 primitive 的 tick inputs 保存 snapshot：

- input 未变化时跳过对应 `tick_compute()`；
- input 变化时执行原有状态计算；
- commit 仍保持两阶段更新，不改变 state ownership；
- commit 后只对实际变化的输出传播 dirty。

### 3.8 module-pipeline 辅助能力

分支还实现了可选的：

```text
--module-pipeline=off|analyze|rewrite
```

其用途是：

- 用 instance-aware graph 区分真实跨实例组合环和“整 instance 原子化”造成的伪 SCC；
- analysis 模式只输出可审计 summary；
- rewrite 模式在受支持场景生成最少 stage，并重写 callsite；
- 重写后重新生成并校验 change schedule metadata；
- 重复运行保持幂等；
- metadata 过期、真实组合环或 unsupported multi-callsite 时失败。

默认模式仍是 `off`。当前 rewrite 是受约束实现，不应描述成对任意大型层次设计都已
完成通用最优拆分。

### 3.9 明确删除的功能

开发过程中曾加入 simulator statistics、timing JSON 和 A/B 分析工具。最终提交
`7d84e293` 删除了这些运行时统计字段、环境变量、dump API、相关 gate 和统计分析
脚本，以避免测量代码进入正式生成模型。

dirty-only hard break 进一步删除了固定比较
`current/dag-always/dag-dirty` 的 `run_change_driven_ab.py` 及其单测。未来性能
对比应使用独立 baseline/candidate 二进制，不能为测量重新引入 emitter mode。

### 3.10 当前实现边界

当前实现更准确的描述是：

```text
拓扑扫描或 fallback 控制流
  + 本地 fused-comb dirty guard
  + instance/primitive/state input cache
  + 语义输出变化后的本地 comb fanout
```

它不是覆盖全部 schedule node 的全图 worklist scheduler：

- full-topology 路径仍会线性访问计划中的节点；
- dirty bit 只直接控制本地 fused comb；
- instance/primitive 节点仍会被访问，再由各自 input cache 决定是否执行主体；
- emitter 从 schedule fanout 中只提取本函数内 `pyc.comb` consumer。

## 4. 实现细节

### 4.1 编译期 pipeline

与 change-driven 直接相关的后端顺序为：

```text
前端与通用 lowering / cleanup
  → instance-aware / local comb-cycle gates
  → 可选 module-pipeline analyze/rewrite
  → fuse-comb（或 static comb partition）
  → canonicalize / CSE / dead cleanup
  → comb memoizable / partition checks
  → 再次检查 comb cycles
  → pyc-plan-change-driven-schedule
  → pyc-check-change-driven-schedule
  → flat-types / no-dynamic / logic-depth gates
  → pyc-cpp-placement（仅 C++）
  → C++ / Verilog emission
```

`pyc-plan-change-driven-schedule` 和 verifier 位于 emitter 之前，保证 emitter
只消费已验证计划。

### 4.2 schedule 构造算法

`buildChangeDrivenSchedule()` 使用统一 `FunctionCombDepGraph`：

1. 构建函数内 canonical value dependency graph；
2. 先要求 same-tick graph 可稳定拓扑排序；
3. 收集每个 schedule op 的每个 result 作为 candidate；
4. 从 candidate result 反向穿过普通 value nodes，找到最近的 schedule
   predecessor；
5. 形成 candidate-level predecessor/successor graph；
6. 使用按 textual index 排序的 ready queue 做确定性 Kahn 拓扑排序；
7. 分配连续 `id/slot`；
8. 以最长前驱路径计算 `rank`；
9. fanout 排序后写入 IR；
10. 在函数上写 node/edge/rank summary。

同一输入重复运行应生成字节稳定的 metadata。

### 4.3 独立 verifier pass

`pyc-check-change-driven-schedule` 不信任已有 metadata，而是重新调用
`buildChangeDrivenSchedule()`，逐项比较：

- schema 和 summary counts；
- 每个 result 的 id/rank/slot；
- fanout 长度、内容、排序和 forward-only 条件；
- unsupported/non-node op 上不得出现 schedule attrs。

因此手工篡改 summary、fanout 或把 metadata 放错 op 会在 emission 前失败。
这里的“独立”是指 verifier pass 独立于 IR 上已有 metadata 重新计算；planner 和
verifier 复用同一个 canonical builder，因此这项检查不能发现 builder 自身的共模错误。

### 4.4 emitter 只读计划

`CppEmitter.cpp::readEmissionSchedule()`：

- 要求 `pyc.change_schedule.v1`；
- 检查 id、rank、slot 唯一且在范围内；
- 检查 fanout 已排序且只指向后继；
- 校验实际 edge/rank 数与 summary 一致；
- 建立 `Value → schedule node` 映射。

它不会重新构造依赖边。这样 MLIR planner/verifier 是语义来源，C++ emitter 只是
计划消费者，避免 backend drift。

### 4.5 direct fanout 生成

`emitMetadataCombFanout()` 从 producer result 的 metadata 读取 fanout，只保留
本地 `pyc.comb` consumer，生成类似：

```cpp
static constexpr std::array<unsigned, N> _pyc_direct_fanout_x{{...}};
for (unsigned consumer : _pyc_direct_fanout_x)
  _pyc_mark_comb_dirty(consumer);
```

它在以下位置被调用：

- fused comb 输出实际变化；
- child instance 某个输出实际变化；
- primitive 可见输出实际变化；
- register/memory/FIFO/CDC commit 报告输出变化。

### 4.6 first-eval 和边界安全

构造模型时，所有本地 comb 都先标记 dirty，保证第一次 `comb()` 完整求值。

dirty 传播只替代可证明由 schedule producer 驱动的输入。顶层参数、外部直接写入
和其他非节点边界继续通过 snapshot 比较。这一混合策略避免要求所有 `Wire`
赋值都立刻接入统一 version clock。

这是保守实现，与 Decision 0108 的最终目标仍有差距：0108 要求从外部输入 fanout
推导 initial dirty set，而不是把所有本地 comb 先标 dirty。当前实现优先保证首次
求值完整正确，尚未把外部输入建模为统一 versioned handle。

### 4.7 fallback 的现状

当前实现没有完全删除 fallback：

- 能建立 full topology 时执行单遍拓扑路径；
- 不能建立时仍保留 `eval_fixpoint_fallback_path()`；
- 可通过 `PYC_SIM_FAST` 选择 SCC worklist fast path；
- dirty guards、instance/primitive cache 和 semantic publish 同样减少 fallback
  内部的无效工作。

因此本分支的实际定位是“本地 fused-comb 的 change-driven 跳过与传播机制 +
受约束模块流水”，
不是“所有设计都已强制转换为无 fallback 的单遍 DAG”。

同理，它还不是 Decision 0109 所描述的“全图 dirty queue 处理到空”为唯一执行
模型；现有 full-topology/fallback 框架仍负责访问 instance、primitive 和状态节点。

## 5. 实现后的执行流程

### 5.1 编译阶段

```text
Python DSL / .pyc
  ↓
frontend contract 与静态 lowering
  ↓
CombDepGraph：统一值依赖、跨实例摘要、cycle legality
  ↓
可选 module-pipeline：分析或重写伪 SCC
  ↓
fuse/partition comb
  ↓
PlanChangeDrivenSchedule
  ├─ node / rank / slot
  ├─ direct fanout
  └─ function summary
  ↓
CheckChangeDrivenSchedule 独立重算并校验
  ↓
CppPlacement
  ↓
CppEmitter 读取 metadata，生成 cache、dirty set 和 wake-up 代码
```

### 5.2 第一次 `comb()`

```text
构造 SimObject
  → 所有 fused comb 标记 dirty
  → instance/primitive/state cache valid=false
  → comb()/eval()
       → full-topology 或 fallback 调度
       → comb guard 看到 first-eval，执行
       → 输出按语义变化发布
       → direct fanout 标记下游 dirty
       → 下游在其拓扑位置执行
  → 顶层输出发布
```

### 5.3 后续输入变化

```text
外部修改顶层 Wire
  → boundary snapshot 在对应 comb 被访问时发现变化
  → 执行该 comb
  → 输出不变：传播停止
  → 输出变化：只 mark metadata 指定的直接 consumer
  → 重汇合 fanout 的重复 mark 自动合并
```

### 5.4 instance 路径

```text
父模块到达 instance 节点
  → 比较输入 cache
  → 输入不变：跳过 child eval
  → 输入变化：执行 child eval
       → 对每个可见输出做前后比较
       → 仅变化输出唤醒本地 direct-comb consumers
  → fallback changed flag 使用语义输出变化
```

### 5.5 primitive 路径

```text
父模块到达 primitive 节点
  → 比较 primitive input cache
  → 输入不变：跳过 primitive eval
  → 输入变化：执行 primitive eval
       → 对可见输出做前后比较
       → 仅变化输出唤醒本地 direct-comb consumers
  → fallback changed flag 仍由 input-cache miss 保守驱动
```

primitive 的输出比较控制 dirty fanout，但尚未像 instance 一样把 fallback
收敛条件完全改为语义输出变化。

### 5.6 一个完整 `step()`

```text
1. comb()
   - 传播本周期新输入
   - 形成正确的 next-state 输入和 TICK-OBS

2. tick()/tick_compute()
   - dirty 模式比较 state/instance tick input snapshots
   - 输入不变的状态计算可跳过

3. commit()/tick_commit()
   - 两阶段提交状态
   - primitive 返回 changed bool/bitmask
   - 只对实际变化的 Q/read-data/FIFO 输出执行 direct wake
   - stateful child 的 eval cache 失效；纯组合 child cache 保留

4. comb()
   - 只执行 commit 唤醒或边界输入变化影响的组合区域
   - 形成 XFER-OBS 和 step 返回后的稳定输出
```

## 6. 主要文件

| 文件 | 职责 |
| --- | --- |
| `compiler/mlir/include/pyc/Transforms/ChangeDrivenSchedule.h` | schema、plan 数据结构、统一 builder 接口 |
| `compiler/mlir/lib/Transforms/PlanChangeDrivenSchedulePass.cpp` | canonical schedule 构造和 metadata 写入 |
| `compiler/mlir/lib/Transforms/CheckChangeDrivenSchedulePass.cpp` | 独立重算和严格 verifier |
| `compiler/mlir/lib/Transforms/CombDepGraph.cpp` | 统一组合依赖图 |
| `compiler/mlir/lib/Emit/CppEmitter.cpp` | metadata 消费、cache、dirty 传播、状态 wake |
| `compiler/mlir/include/pyc/Emit/CppEmitter.h` | C++ emitter 公共配置；更新策略固定为 dirty |
| `runtime/cpp/pyc_change_scheduler.hpp` | fixed-capacity dirty set 与 version/publish 工具 |
| `runtime/cpp/pyc_*` primitives | eval cache 和 commit change reporting |
| `compiler/mlir/lib/Transforms/ModulePipelinePass.cpp` | 可选跨实例 SCC 分析/重写 |
| `compiler/mlir/tools/pycc.cpp` | CLI 参数和 pass 接线 |
| `compiler/frontend/pycircuit/cli.py` | build CLI；不再暴露 comb update 模式 |

runtime 源文件及两个 public include 副本保持一致：

- `runtime/cpp/pyc_change_scheduler.hpp`
- `include/pyc/cpp/pyc_change_scheduler.hpp`
- `include/cpp/pyc_change_scheduler.hpp`

## 7. 验证结果

以下结果在 `feat/change-driven-scheduler` @ `7d84e293` 加 dirty-only 工作树上
于 2026-09-23 复测。

### 7.1 通过

| 验证 | 结果 | 覆盖内容 |
| --- | --- | --- |
| `cmake --build .pycircuit_out/toolchain/build --target pycc -j2` | PASS | 独立分支 pycc 编译链接 |
| `tests/runtime/run_change_scheduler.sh` | PASS | 三份 public header、DirtyBitset、semantic publish、version wrap/rebase |
| `compiler/mlir/test/comb_dirty_scheduler_smoke.sh` | PASS | dirty-only CLI hard break、direct fanout、reconvergence、state/instance/primitive、无 stats 残留 |
| `compiler/mlir/test/module_pipeline_smoke.sh` | PASS | analysis/rewrite、真实环拒绝、最少 stage、幂等、metadata、安全失败 |
| `compiler/mlir/test/instance_vector_cache_smoke.sh` | PASS | nested vector instance cache 精确为 48 packed words |

### 7.2 部分失败

`compiler/mlir/test/change_schedule_gate.sh` 的主体检查通过：

- reg feedback 合法、真实组合环被拒绝；
- 两次 planner 输出稳定；
- child/top 的 node、edge、rank 和 instance metadata 符合预期；
- tampered schedule 被 verifier 拒绝。

脚本最终因诊断文本期望不一致退出 1：

```text
期望：schedule fanout size mismatch
实际：change-driven schedule summary does not match canonical graph
```

也就是说 tampered IR 已被正确拒绝，但 verifier 先在 summary 层失败，测试仍要求
后续 fanout 层诊断。该问题属于 gate 诊断顺序/fixture 期望漂移，不代表非法
schedule 被接受；仍需单独修正测试或固定诊断优先级。

### 7.3 代码规模

相对 `upstream/main` @ `27b226cd`，当前 tracked 工作树（包含 change-driven 所依赖的
CombDepGraph、module pipeline、C++ placement/PCH 等前置工作）为：

```text
89 files changed
10,926 insertions
618 deletions
```

该数字不是纯 scheduler LOC，不能用于估计 scheduler 本身复杂度。

### 7.4 性能结论的边界

当前可以确认：

- 原路径存在显著重复求值，旧基线顶层 fallback 占 `eval()` 约 99.91%；
- change-driven 的编译期计划、runtime 数据结构、组合/层次/状态传播已通过功能门禁；
- C++ emitter 已固化为唯一 dirty 策略，不再存在模式开关；
- simulator 内建统计已从最终分支删除。

当前不能严谨宣称：

- Davinci qwen3-14b 端到端提升了某个百分比；
- 所有层次设计都已消除 fallback；
- module-pipeline rewrite 已覆盖任意多 callsite 或所有 primitive 拆分；
- dirty-only 实现在所有 workload 上都一定快于旧 always reference。

原因是最终实现没有完成同一设计和工具链下的旧 baseline binary 与 dirty-only
candidate binary 正式 A/B。性能收益必须使用两个独立构建产物测量，不能重新加入
运行时模式开关。

### 7.5 当前值语义限制

`publishIfChanged()` 当前依赖二值 `Wire::operator==`。runtime 注释已明确：只有
未来 `Wire` 存储实现 `(value_bits, known_mask, z_mask)` 并让 equality 覆盖三者后，
该 helper 才满足 Decision 0129 的 X/Z 四值变化检测要求。本文所称“语义变化”
均限定为当前二值 C++ runtime。

## 8. 已知限制与后续工作

1. 修正 `change_schedule_gate.sh` 中 tampered fixture 的诊断优先级期望。
2. 在固定 Davinci commit、trace、编译器和宏配置下运行正式 A/B：
   - 1 次预热；
   - 7 次交错测量；
   - median 和 p95；
   - 输出、状态、assert、TICK-OBS/XFER-OBS 一致性。
3. 将 callee output-to-input 精确摘要硬化到 dialect/metadata，减少 instance
   输入变化时的保守 child eval。
4. 扩展 module-pipeline rewrite 对多 callsite 和更多复杂状态 ownership 的覆盖。
5. 在模块 DAG 证明充分后评估彻底删除正常路径的 fixed-point fallback；在此之前
   fallback 仍是当前实现的一部分。
6. 更新 `docs/MLIR_PASS_PIPELINE.md`，其当前表格尚未完整列出 module-pipeline、
   plan/check-change-driven-schedule 等新 pass。
7. 从外部输入 fanout 计算首次 dirty set，完成 Decision 0108，而不是首次把所有
   本地 comb 标 dirty。
8. 将调度扩展为覆盖 instance/primitive/state 的全图 worklist，并处理到队列为空，
   逐步完成 Decision 0109。
9. 实现 X/Z 三掩码值存储和完整 equality/version/signature，完成 Decision 0129。

## 9. 环境与复现命令

前置条件：

- Python frontend 可由 `compiler/frontend` 导入；
- 系统有 C++17 compiler、CMake、Ninja、LLVM/MLIR 19。

从干净 checkout 配置和安装工具链：

```bash
LLVM_DIR="$(llvm-config-19 --cmakedir)"
MLIR_DIR="$(dirname "$LLVM_DIR")/mlir"

cmake -G Ninja -S . -B .pycircuit_out/toolchain/build \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_INSTALL_PREFIX="$PWD/.pycircuit_out/toolchain/install" \
  -DLLVM_DIR="$LLVM_DIR" \
  -DMLIR_DIR="$MLIR_DIR"

ninja -C .pycircuit_out/toolchain/build pycc pyc-opt pyc4_runtime
cmake --install .pycircuit_out/toolchain/build \
  --prefix "$PWD/.pycircuit_out/toolchain/install"
```

完整依赖说明见 `docs/getting-started/installation.md`。

```bash
git switch feat/change-driven-scheduler

cmake --build .pycircuit_out/toolchain/build --target pycc -j2

tests/runtime/run_change_scheduler.sh
compiler/mlir/test/comb_dirty_scheduler_smoke.sh
compiler/mlir/test/module_pipeline_smoke.sh
compiler/mlir/test/instance_vector_cache_smoke.sh
```

单独检查 schedule gate：

```bash
compiler/mlir/test/change_schedule_gate.sh
```

当前预期是主体检查完成、tampered IR 被拒绝，但因上述诊断文本顺序差异返回 1。

正式性能 A/B 需要 baseline/candidate 两套独立二进制固定并校验：

- 相同设计、trace、编译器、优化等级和 cache 宏；
- 相同输出、状态、assert 和 TICK-OBS/XFER-OBS；
- 唯一差异是旧基线实现与 dirty-only candidate。

旧基线的外部 Davinci 工作树、脏 diff 和临时产物不能视为稳定复现环境；其完整
provenance 记录在 `docs/module-fallback-timing-measurement-plan-20260922.md`。
