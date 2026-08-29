# C++ 变更驱动组合逻辑求值需求分析与实施规划

## 背景与目标

pyCircuit 当前的 C++ 功能仿真采用“编译后直接执行”的方式：编译器将硬件组合逻辑排成拓扑顺序，并在每个组合稳定点调用生成的 `eval()`。这保证了单次前向求值的确定性，但即使某一段组合逻辑的所有外部输入和其读取的寄存器/存储器输出均未变化，该段代码仍会被重复执行。

本功能的目标是在 MLIR 中分析每个 `pyc.comb` 的直接输入与输出，并把分析结果带入 C++ 发射器。生成的 C++ 对每个 `eval_comb_N()` 保存上次观察到的输入 `Wire` snapshot：首次调用或任一输入语义值不同才执行该 comb body；全部输入相同则直接返回并保留上次输出。comb 执行后的每个跨 comb/primitive/instance/return 边界输出须与旧值精确比较，实际变化时才标记 `changed=true`。本期不引入全局 ValueSlot、version、fanout dirty queue 或全局事件调度器。

本期不改变 Python 前端接口、Verilog 发射结果、硬件语义或模块层级。它是 C++ 仿真的可禁用性能优化，而不是在后端私自改变语义：可求值实例、端口依赖、fanout 与安全资格均由 MLIR pass 明确分析和验证。

## 2026-08-18 本地寄存器 commit-driven 增量

当前实现新增 `--comb-reg-update=poll|commit`，但不增加 MLIR attribute、
activity-plan 或 verifier。C++ emitter 只识别最终 `pyc.comb` 的直接、
同 function `pyc::RegOp::q` 输入，建立排序去重的
`RegOp → direct consumer comb ID`；alias、wire/assign、extract、packed
路径、instance、FIFO、memory 和 CDC 继续使用 boundary snapshot。

scalar/vector `pyc_reg::tick_commit()` 现在返回 `bool`：无 pending 或
`qNext == q` 返回 false，否则在保持原提交行为后返回 true。commit 模式
消费该返回值并复用 `_pyc_comb_active_words` 唤醒直接 consumer；后续
comb 仍通过精确输出比较逐级传播。新增统计为 `reg_commit_checks`、
`reg_semantic_changes` 和 `reg_fanout_enqueues`。没有轮询边界的 inactive
comb 会在调用点跳过 helper call，避免仅为消费 inactive bit 进入函数。

`poll` 保持默认。逐周期状态 gate 的 poll/commit 两路结果相同，包含首次
eval、`en=0`、同值 reset commit、实际改值和 post-commit comb。8-tap
digital-filter 的 7 次、每次 500,000 cycle A/B 中，poll 和 commit 的 C++
中位吞吐分别为 6.486 M/s 和 6.552 M/s（1.010x）；64 direct-consumer、
零状态活动 microbenchmark 的中位 eval 延迟分别为 68.1003 ns 和
64.2523 ns（1.060x）。后者未达到 10% 默认切换门槛，因此 commit 保留为
显式实验模式，避免把未达到验收条件的优化设为默认。

## 项目现状与执行流程定位

Python 前端先生成 `pyc` MLIR；`pycc` 在 `compiler/mlir/tools/pycc.cpp` 运行合法性、组合规范化、组合融合、组合环检查、类型检查和 C++member placement，然后调用 C++ emitter。`pyc-fuse-comb` 会把连续的纯组合运算合为 `pyc.comb` region；C++ emitter 再将每个 region 发射为 `eval_comb_N()`，或在完整拓扑图可用时将组合节点直接发射进 `eval_topo_part_N()`。

当前 C++ `eval()` 以拓扑顺序执行所有组合节点。对于包含实例、FIFO 或存储器等 primitive 的层级设计，emitter 已有实例/primitive 输入 snapshot 缓存：子实例输入不变时不调用子实例 `eval()`，并在有状态子模块 commit 后使缓存失效。但是这仍要求父模块每次扫描所有 child 输入，没有从变更输出直接唤醒 fanout 的 dirty queue。`runtime/cpp/pyc_change_detect.hpp` 现有 `ChangeDetector`、`InputFingerprint` 和 `EvalGuard` 基础设施，但其使用仍停留在 RegisterFile C API 的手写 DUT 级 guard；尚未由 MLIR/C++ emitter 自动驱动。

组合求值处于 `comb()`/`eval()` 阶段；`tick()` 只计算下一状态，`commit()`/`transfer()` 提交状态，并且 `step()` 采用 `comb → tick → commit → comb`。因此寄存器、FIFO、memory 和子模块提交后的可见输出必须成为下一轮组合块的变化输入，不能仅依据顶层端口决定是否跳过。

## 相关现有实现说明

- `compiler/mlir/lib/Transforms/FuseCombPass.cpp` 与 `compiler/mlir/include/pyc/Dialect/PYC/PYCOps.td`：`pyc.comb` 是纯组合融合单位；这是本期首选的优化粒度。它比对每一个标量运算插入 guard 的开销更低，也符合现有 emitter 的代码分块方式。
- `compiler/mlir/include/pyc/Transforms/CombDepGraph.h` 与 `lib/Transforms/CombDepGraph.cpp`：已有跨实例组合依赖摘要，用于组合环和逻辑深度 gate。新 pass 应扩展/复用其“状态 primitive 是组合依赖 cut point、InstanceOp 不是 cut point”的原则，产生统一的端口依赖/fanout 计划，而非在 emitter 内重新实现独立数据流语义。
- `compiler/mlir/lib/Emit/CppPlacement.cpp` / `CppPlacementPass.cpp`：决定组合 SSA 值放在 struct 成员还是 `eval_comb_N()` 局部变量。被 guard 的 comb 输入 snapshot 和跨 comb/primitive/instance/return 边界输出必须保持为 struct 成员；pass 和 placement 的契约必须显式验证这一点。
- `compiler/mlir/lib/Emit/CppEmitter.cpp`：目前生成 `eval_comb_N()`、完整拓扑 `eval()`、局部实例/primitive cache、固定点回退路径和 `PYC_SIM_STATS` 计数器。这里将消费 IR 中的 comb 输入/输出计划，生成 per-comb snapshot、`changed` 输出元数据、guard 和统计；现有 emitter-local topology/SCC 图不能成为第二份独立的语义来源。
- `runtime/cpp/pyc_change_detect.hpp`：可复用逐 Wire 精确比较的实现思想；本期优先由 emitter 生成每块 typed 输入快照/比较，而非用 XOR fingerprint 作为唯一判据，避免哈希碰撞影响正确性。
- `designs/RegisterFile/regfile_capi.cpp`：证明低活动率时“输入未变则跳过 DUT `eval()`”可获益，但它是专用 C API wrapper，且每次寄存器提交后强制重新求值；不能直接推广为通用后端方案。
- `../DavinciOO_unify/srcs/core/`：该仓库的模块是 pyCircuit 的大型硬件建模工作负载，普遍通过 `valid/ready/fire` 表示低活动率和 backpressure，适合作为真实性能/回归样例。`../DavinciOO_unify/model/` 的 `SimObject`、`SimQueue` 和事件处理是独立的架构级 cycle model，不是 pyCircuit 生成 C++ 的组合调度器；不能复制其队列或事件语义到本优化中。



### 当前生成 C++ 模块的函数与阶段职责

每个 MLIR `func.func` 会生成一个 C++ `struct`。它包含端口/中间 `Wire` 成员、reg/FIFO/memory 等 primitive 对象、子模块 `unique_ptr`，以及以下函数。不同设计会因是否有 hierarchy、SCC 或组合分块而省略部分 helper，但公开 phase API 保持一致。

| 函数 | 当前职责 | 与本功能的关系 |
|---|---|---|
| 构造函数 | 创建 `pyc_reg`、sync memory 等 primitive，并把模块的 `Wire` 成员以引用绑定为 primitive 端口；创建子模块对象。 | 初始化每个 guardable comb 的输入 snapshot 与结果 changed metadata；不得改变 primitive 对真实 `Wire` 的端口绑定。 |
| `eval_comb_N()` | 执行一个融合后的 `pyc.comb` region；计算加法、mux、比较等纯组合表达式并写回结果 wire。大 region 还会拆为 `eval_comb_N_part_M()`。 | 第一阶段仍可复用为执行单元的组合主体；只有跨调度边界的写回需要经 `update_changed`。 |
| `eval_instance_cached_N()` | 打包/比较父模块准备传给 child 的所有输入 snapshot；首次、输入不同或 cache 失效时复制输入并调用 `child->eval()`；之后总会把 child 输出复制回父模块 wire。 | 保持现有子模块缓存；child 内部的 guardable comb 再独立执行输入 snapshot 检查。 |
| primitive cache helper | 为 FIFO、byte memory、async FIFO 等 primitive 比较输入 snapshot，必要时调用其 `eval()`，然后读取组合输出。 | 保持现有 primitive cache；其输出作为下游 comb 输入时由 comb 自己的 snapshot 检查。 |
| `eval_topo_part_N()` | 当 emitter 能建立完整无环拓扑序时，按该顺序执行一段组合节点；用于控制 C++ 函数大小。 | 可作为 event mode 中已入队组合执行单元的代码主体，但不再代表每次 `eval()` 必跑的全图扫描。 |
| `eval_scc_comp_N()` / `eval_fixpoint_fallback_path()` | 现有复杂/无法完全拓扑排序时的 SCC 或有界固定点回退路径。 | 默认 event plan 之前必须通过 comb-cycle gate；这些函数保留为 reference/debug fallback。 |
| `eval()` | 当前组合入口：全拓扑可用时顺序执行全部节点；否则执行组合 pass 与 primitive/SCC fixed-point 路径；最后将 return 值复制到模块输出端口。 | 保持入口和顺序不变；guardable `eval_comb_N()` 在调用点内部跳过不变输入的 body。 |
| `comb()` | 公开 API，当前只是 `eval()` 的别名。 | 保持 API 和 phase 语义不变；不引入 dirty queue。 |
| `tick_compute()` | 先调用全部 child 的 `tick_compute()`，再调用本地 reg/FIFO/memory/CDC 的 `tick_compute()`；reg 在此检测时钟边沿并把候选值写入私有 `qNext`，不修改可见 `q`。 | 该阶段必须根据最终确认的 phase 语义决定哪些已入队状态执行单元运行；不能在这里过早传播 reg `q` 变化。 |
| `tick()` | 公开 API，当前只是 `tick_compute()` 的别名。 | 保持 API 兼容；新调度需要明确 event-driven tick 与 `comb()` 的分工，并由 API litmus 固化。 |
| `tick_commit()` | 先提交 child，再提交本地 reg/FIFO/memory/CDC；reg 的 `pending` 为真时执行 `q = qNext`。当前还会粗粒度使有状态 child/primitive 的 eval cache 失效。 | 保持 primitive commit 行为不变；提交后的可见值会在下一次下游 comb guard 的输入 snapshot 中被发现。 |
| `commit()` / `transfer()` | 公开 API，当前都只是 `tick_commit()` 的别名。 | 保持 batch commit，不增加 changed/fanout 调度。 |
| `step()` | 高层单周期 API：`comb() → tick() → commit() → comb()`。 | 是 C++/Verilog 逐周期等价的主要入口；优化开关前后必须在此路径验证等价。 |
| `pyc_trace_*()`、`dump_sim_stats()` | 递归注册 trace/probe，或输出 instance/primitive cache 调用与跳过统计。 | trace 保留既有 comb/TICK-OBS/XFER-OBS 语义；统计扩展为 comb guard 的执行、比较和跳过指标。 |

`pyc.reg` 的运行时实现为 `pyc::cpp::pyc_reg<Width>`：`tick_compute()` 在上升沿采样 `rst`、`en` 和 `next`，保存到私有 `qNext`；`tick_commit()` 仅在 `pending` 为真时将 `qNext` 写到模块可见 `q`。因此 `q` 是 changed 设计中最重要的状态输出边界，`qNext` 只是内部临时状态，不需要单独传播。

### 两级层次流水线 litmus example

为直观看到当前父/子模块端口复制、submodule eval cache 和 reg commit 的实际生成 C++，实施时新增独立示例目录 `designs/examples/hierarchical_two_stage_pipeline/`。它不是 changed scheduler 的实现，而是后续正确性、IR fanout 与生成 C++ 检视的最小层次化基准。

结构固定为：

```text
top(a, b, d, clk, rst)
  ├─ stage1(a, b)           // 独立子模块：c_comb = a + b
  ├─ c_q = reg(c_comb)      // 位于 top：打一拍，reset 时为 0
  └─ stage2(c_q, d)         // 独立子模块：e = c_q + d
top 输出：c_q、e
```

对应的时序语义是：在第 N 个上升沿，top 的 reg 采样 `stage1` 由 `a[N] + b[N]` 得到的 `c_comb`；提交后，`stage2` 看到 `c_q = a[N] + b[N]` 并计算 `e = c_q + d[N]`。因此 `a/b` 到 `c_q` 有一拍状态边界；`d` 到 `e` 是当前周期组合路径。

计划新增的文件和职责：

| 文件 | 职责 |
|---|---|
| `designs/examples/hierarchical_two_stage_pipeline/hierarchical_two_stage_pipeline.py` | 定义 `stage1`、`stage2` 两个可实例化子模块，以及 top；top 用 `m.instance(...)` 连接子模块，并在 top 内部分配唯一的 `domain.signal`/reg 保存 `c_comb`。 |
| `designs/examples/hierarchical_two_stage_pipeline/hierarchical_two_stage_pipeline_config.py` | 固定 `width`、smoke/nightly timeout 和 example 元数据。 |
| `designs/examples/hierarchical_two_stage_pipeline/tb_hierarchical_two_stage_pipeline.py` | 驱动 reset、两组不同的 `a/b/d` 输入；断言 `c_q` 的一拍更新与 `e = c_q + d`，并让 C++/Verilator 均执行。 |

验收点：

1. 发射 MLIR 中有 top、`stage1`、`stage2` 三个 `func.func`，并有两个 `pyc.instance`，不能被默认 inline/flatten 掩盖。
2. C++ 输出含三个 struct；top 通过 `unique_ptr` 持有两个 child，分别生成 `eval_instance_cached_*()` helper。
3. reset 后 `c_q=0`；输入 `a=1,b=2,d=10` 的上升沿提交后，`c_q=3,e=13`。
4. 将输入改为 `a=4,b=5,d=20`：下一个提交后，`c_q=9,e=29`；由此证明 `c_q` 的状态边界与 `stage2` 当前 `d` 输入都在 C++ 运行路径中生效。
5. 使用 `--hierarchy-policy=instantiate` 或等价的 non-inline 编译配置生成检查用 C++；普通回归还应覆盖项目默认配置，避免示例只在特殊模式下可用。

## 需求与验收标准



### 功能需求

1. 在 C++ 发射前新增 `pyc-plan-comb-inputs`（暂定名）MLIR pass。它对每个 `pyc.comb` 记录稳定的直接输入列表（`comb.getInputs()`）、输出列表（`comb.getResults()`/`pyc.yield`）及结果是否跨调用边界。
2. 对每个可 guard 的 `eval_comb_N()` 生成 `CombCache_N`：`initialized` 与每个直接输入的 `Wire<width>` snapshot。首次执行后捕获快照；以后只有任一当前输入与 snapshot 精确不同时才执行 comb body。
3. 所有 C++ emitter 支持的 PYC comb 输入类型均为 `pyc::cpp::Wire<N>`，其中 integer 使用自身位宽、clock/reset 使用 `Wire<1>`。这不是允许继承 `Wire` 的理由：`Wire` 是 `Bits<N>` type alias，primitive 持有 `Wire&`，赋值/`setWord()` 可绕过子类方法。`changed` 必须是与 Wire 分离的 metadata。
4. comb body 执行后，对每个跨调用边界输出用 emitter 生成的 `assign_and_mark_changed(Wire<N>& dst, const Wire<N>& next, bool& changed)` 写回；仅在精确比较发现新旧值不同时赋值并置 `changed=true`。该 flag 用于统计和未来优化，不作为本期跳过条件；本期跳过条件唯一是该 comb 的输入 snapshot。
5. `pyc.reg`、FIFO、memory、CDC、子模块输出仍按现有 `eval()`/`tick_compute()`/`tick_commit()` 时序更新；它们只要作为某个 comb 的直接输入，就自然由该 comb 的 snapshot 检查覆盖。本期不修改 primitive runtime 或引入 primitive changed/fanout 调度。
6. 对含 `pyc.assert`、无法保持输出、未知 op、固定点/SCC fallback 或不是 `pyc.comb` region 的执行路径，保持无条件执行；只有经 pass 验证的纯 `pyc.comb` 才允许 guard。
7. 提供默认启用、编译期可关闭的开关，以及 `PYC_SIM_STATS` 中 comb 执行次数、comb snapshot skip、输入比较次数和实际输出变化次数统计。



### 非功能需求与验收

- **语义等价**：优化开/关在同一输入序列下得到相同端口值、寄存器/memory 状态、assert 结果和 trace 事件序列。
- **确定性**：同一 IR 的计划属性与生成 C++ 顺序稳定；不引入线程、全局事件队列或 delta-cycle 新语义。
- **零错误跳过**：输入 snapshot 使用精确 `Wire` 比较；哈希可用于快路径但不能单独决定“不变”。
- **性能**：在 DavinciOO 的一个停顿/空闲比例可控的 pyCircuit 用例和本仓 RegisterFile 用例上，分别测量 baseline 与开启优化后的每周期耗时；只有在低活动率 workload 获益且 100% active workload 的退化不超过预先记录的噪声阈值时才默认启用。
- **可观测性**：生成统计至少包含 `comb_eval_calls`、`comb_cache_skips`、`comb_input_compares`、`comb_output_changes` 与 fallback 次数，并可通过关闭开关产生对照结果。



## 方案设计



### 模块边界、输入与输出

新增 `pyc-plan-comb-inputs`（暂定名）function pass，运行在融合、最终 canonicalize/CSE 和合法性检查之后、`pyc-cpp-placement` 之前。

输入是已平坦化、已检查无组合环的 `func.func` 及其中的 `pyc.comb` region；输出是：

- module/function 级启用属性和计划 schema 版本号；
- 每个 `pyc.comb` 的稳定 plan id；
- 与 region block arguments 一一对应的 `comb.getInputs()` 有序列表；
- 与 `pyc.yield` 一一对应的 `comb.getResults()` 有序列表；
- 每个结果是否跨 helper/primitive/instance/return/probe 边界、因而必须作为 struct member 持久保存的属性；
- 对不可 guard 的 region/执行路径的原因码；
- 对无法计划者的原因码或保守的 reference-path 标记。

pass 自身不改变电路数据流、op 顺序或 Verilog 语义，只产生 C++ 发射契约。C++ emitter 根据该契约为每个 `eval_comb_N()` 生成 `CombCache_N`，其中含 `initialized` 和输入 Wire snapshot；它不生成 dirty queue 或全局 fanout 表。

### comb 输入 snapshot 与输出 `changed` 规则

当前 emitter 的 `cppType()` 将 PYC integer 统一发射为 `Wire<width>`，并将 `!pyc.clock`/`!pyc.reset` 发射为 `Wire<1>`。因此一个已验证的 `pyc.comb` 可生成下述等价结构（实际字段按每个输入的真实宽度展开）：

```cpp
struct CombCache_12 {
  bool initialized = false;
  Wire<32> input_0{};
  Wire<32> input_1{};
  Wire<1> input_2{};
};
CombCache_12 comb_cache_12{};
bool pyc_comb_12_result_0_changed = false;
```

`eval_comb_12()` 的 guard 顺序为：

1. 比较当前 `comb.getInputs()` 映射的 Wire 与 cache 中同位置 snapshot；
2. 若 `initialized=true` 且全部相同，递增 `comb_cache_skips` 并立即返回；
3. 若任一不同，执行原有 comb body；
4. 将当前输入复制到 snapshot，置 `initialized=true`；
5. 对每个跨调用边界 result，以 `assign_and_mark_changed` 比较旧 Wire 和新计算值；只有不同才写回并置结果的 `changed=true`。

`changed` 不是 `Wire` 的子类字段，也不需要给每一个 Wire 生成。它仅属于必须跨 helper 调用保存的 comb result，并在下一次 `eval()` 开始前清零。因为下游 comb 自己比较输入 snapshot，本期不使用该 flag 作为 fanout 或调度条件。

### 上下游关系与数据/控制流

1. `pycc` 完成 frontend contract、SCF lowering、wire/dead-state 清理、comb canonicalize、fusion、最终 CSE、comb-cycle/no-dynamic/flat-type/depth gate。
2. `pyc-plan-comb-inputs` 遍历融合后的 `pyc.comb`，读取 operands、region block arguments、yield 与 results，验证其一一对应、输入类型可发射为 `Wire<N>`、输出 storage 能跨跳过调用保持。
3. pass 对含不支持 op、非单 block region、assert 或无法持久化结果的 comb 标注 `guardable=false`；C++ emitter 对它们维持无条件调用，不在后端猜测。
4. `pyc-cpp-placement` 将每个 guardable comb 的 snapshot 输入和跨调用边界结果固定为 struct storage；纯 comb 内临时值仍保持函数局部变量。
5. `eval()` 保持现有拓扑顺序；它仍会依次调用 `eval_comb_N()`，但每个 guardable helper 先比较自己的直接输入 snapshot，输入全不变时跳过其内部运算。
6. `transfer()`/`commit()` 保持原有 batch 语义。reg `q`、FIFO、memory、CDC 或子模块输出在 commit/eval 后若成为下游 comb 输入，会在下次调用下游 comb 时由 snapshot 自动发现，不增加 primitive 改写或 dirty queue。
7. 当前 SCC/fixed-point 与非-comb op 发射路径保持无条件/reference 行为；本期 guard 只覆盖经 MLIR 验证的 `pyc.comb`。



### 文件和接口改动清单


| 文件                                                                            | 计划改动                                                                                                      | 影响                         |
| ----------------------------------------------------------------------------- | --------------------------------------------------------------------------------------------------------- | -------------------------- |
| `compiler/mlir/include/pyc/Transforms/Passes.h`                               | 声明计划 pass factory。                                                                                        | 让 pycc/pyc-opt 可调用。        |
| `compiler/mlir/lib/Transforms/CombInputPlanPass.cpp`（新增） | 分析每个 `pyc.comb` 的直接 inputs/results、storage 边界和 guardable 条件，写入有 schema 版本的 IR 属性。 | 新的 MLIR 级发射契约。 |
| `compiler/mlir/CMakeLists.txt`                                                | 将新 pass 编入 `pyc_transforms`。                                                                              | 构建集成。                      |
| `compiler/mlir/tools/pycc.cpp`                                                | 在最终组合优化及 legality gate 后、C++placement 前接入 pass；增加 C++ 优化开关。                                               | C++路径生成计划；非 C++ emit 不受影响。 |
| `compiler/mlir/lib/Emit/CppPlacement.cpp` 与 `include/pyc/Emit/CppPlacement.h` | 将 guardable comb 的 snapshot 输入和跨调用 result 设为 struct storage，并在不可满足时给出诊断。 | 跳过后输出值仍有效。 |
| `compiler/mlir/lib/Emit/CppEmitter.cpp` | 验证/消费 plan；生成 per-comb cache、guard、输出 changed flag、reference-mode 开关和统计。 | 生成 C++ comb 级条件执行。 |
| `runtime/cpp/pyc_change_detect.hpp` | 增加最小的精确 `Wire` compare/copy helper（若 emitter 重复逻辑需要）；不引入 ValueSlot、queue 或 Wire 子类。 | 运行时支持，保持 STL-only。 |
| `flows/tools/perf/run_perf_smoke.py`                                          | 增加 baseline/optimized 对照模式、重复次数、统计归档和阈值检查。                                                                | 可复现性能证据。                   |
| `designs/RegisterFile/` 与新增/选定 DavinciOO 集成用例                                 | 添加低活动率和全活动率可控 workload，或调用既有测试驱动。                                                                         | 功能等价与性能验证。                 |
| `docs/simulation.md` | 实施后更新为“编译器自动 per-comb 输入 snapshot guard”，移除“仅手写 wrapper”表述。 | 文档与实际行为一致。 |




### 边界条件、错误处理与兼容性

- **首次调用、重置、对象重建**：每个 comb cache 的 `initialized=false`，因此首次进入 `eval_comb_N()` 必定执行；reset 仍保持 Decision 0028 的 tick/commit 语义。
- **显式** `comb()` **API**：继续调用 `eval()`；每个 guardable comb 对当前 Wire 输入执行自己的 snapshot 检查，不依赖 `step()`。
- **状态写入但值相同**：下游 comb 输入 snapshot 相同，因此跳过；Write/trace 的既有“写入”和“值变化”区分不变。
- **实例与状态 primitive**：保持既有 runtime 与 emitter-local invalidation；其可见输出是 downstream comb guard 的普通 Wire 输入。
- **assert/probe/打印**：assert 是 simulation-visible effect；probe/trace 有 comb、TICK-OBS、XFER-OBS observation contract。计划 pass 必须将它们分类，scheduler 不得因 skip 漏掉要求的观测。
- **组合环、SCC、late assign**：组合环依法拒绝；旧 SCC/fixed-point 保持为 reference/debug fallback，禁止成为默认 event plan 的收敛语义。
- **X/Z 与宽值比较**：comb snapshot equality 必须遵守 Decision 0117 的 `value_bits + known_mask` 语义；fast signature 只作预检。
- **编译开关/回退**：默认开关可在构建时禁用；关闭后走现有全求值 reference path，以便逐周期二分和 C++/Verilog 比较。
- **兼容性**：不改变 `.pyc` 用户可见语法、端口、C++ public `comb/tick/commit/transfer/step` API，也不影响 Verilog emitter。



## 与既有文档和约束的一致性检查

检查范围：

- `docs/updatePLAN.md`：要求 gate-first、MLIR 级语义/验证，禁止 backend-only 语义修复。
- `docs/rfcs/pyc4.0-decisions.md` 的 Decision 0001/0011：保留模块实例对应 SimObject 和 module 内组合逻辑可扁平化；本方案不改层级。
- Decision 0015：保持单线程确定性，不引入任务并行或全局事件队列。
- Decision 0026–0028：保持 `comb → tick → commit`、显式 phase API 和 reset 输入语义。
- Decision 0103–0109：这些决定定义未来实例端口级 event scheduler。本期只实现 module 内的 comb snapshot guard，不实现或替代其中的 version/fanout/dirty-queue runtime 契约。
- Decision 0114/0115/0117/0121/0122：memory/reset、X-aware equality/version 与 observation point 必须由 dialect/计划 pass 明确建模。
- Decision 0127/0128/0132/0134/0135：CombDepGraph、无环性、元数据和跨实例分析是该计划的前置 gate。
- `docs/simulation.md`：认可 change detection 的性能方向，但其“当前已实现自动 per-eval guard”的表述与源代码不完全一致；源码证据显示通用 emitter 目前仅缓存实例/primitive，RegisterFile 使用手写 C API guard。本实施会使文档中“自动块级 guard”的描述成为事实。
- `../DavinciOO_unify/AGENTS.md`：DavinciOO 的 `srcs/core` 是可用的真实 pyCircuit workload；其 `model/` 是独立 cycle model，故仅取 workload/valid-ready 停顿特征，不复制其 SimQueue/事件机制。

结论：本期的 `pyc.comb` 输入 snapshot guard 是局部、保守的 C++ 代码生成优化；它不改变现有 phase、primitive 或 module hierarchy 语义，也不与 RFC 0104–0106 的未来 event scheduler 契约冲突。

## 测试与验证计划



### 正确性

1. **IR plan 单元测试**：构造多个 `pyc.comb`，验证 inputs/block arguments、yield/results 的一一对应、稳定排序、跨调用结果 storage，以及含 assert/未知 op 的保守拒绝。
2. **发射文本与运行时单元测试**：验证 `CombCache_N`、首次必执行、精确 Wire equality、任一输入变化执行、全部输入不变跳过、边界 output changed 标记和禁用开关。
3. **C++ 行为测试**：
  - 首次 comb 必执行；全部直接输入不变时 body 不执行、输出保持；
  - 任一直接输入变化时 body 执行；
  - 上游 comb 执行但输出不变时，下游 comb 输入 snapshot 不变、下游 body 跳过；
  - reg、FIFO、sync memory 和有状态子实例 commit 后，只有直接读取其输出的 comb 在下次 `eval()` 中执行；
  - 连续调用 `comb()`、`step()`、reset helper 与显式 tick/commit 的结果与关闭优化版本逐周期一致；
  - assert、SCC/fallback、含 primitive 的不安全路径不会被错误跳过。
4. **回归**：`bash flows/scripts/run_examples.sh`、`bash flows/scripts/run_sims.sh`，并至少选择一个 DavinciOO `srcs/tests` 模块经其既有 pyCircuit 路径进行 C++/Verilator 对照。



### 性能基线与通过标准

实施前后均在同一主机、相同编译器、`-O2 -DNDEBUG`、固定 CPU governor/无其他重负载条件下执行；每个 workload 预热一次，连续运行 7 次，记录中位数、最小值、p95 和总 cycles。脚本输出 JSON 到 `docs/gates/logs/<run-id>/`，记录 commit、命令、编译选项、机器信息与运行统计。

基准集：

1. RegisterFile：使用现有可控 active percentage workload，覆盖 100%、50%、10%、1% 活动率。
2. DavinciOO：选择一个有 `valid/ready` backpressure 的真实 module/integration workload，分别运行持续活动和人为稳定输入/停顿场景。
3. 一个层级 + 有状态 primitive 例子，避免只证明纯组合扁平网表。

通过阈值（实施前写入脚本配置，可按基线噪声调整）：

- 所有正确性和跨后端回归通过；
- 低活动率（≤10%）至少一个代表 workload 的中位数 cycles/s 提升 ≥10%；
- 100% active workload 的中位数退化不超过 3%；
- 统计显示低活动率场景确有非零且可解释的 comb snapshot skip，关闭开关时 skip 为零。

建议执行命令：

```bash
bash flows/scripts/pyc build
python3 flows/tools/perf/run_perf_smoke.py --profile release --sim-mode cpp-only --perf-repeats 7
bash flows/scripts/run_examples.sh
bash flows/scripts/run_sims.sh
```

实施时还应为 DavinciOO 选择的用例记录精确 `PYCIRCUIT_ROOT`、Python 环境和调用命令；该仓的 `srcs/tests` 依赖此变量。

## 实施步骤

1. 确认融合后 `pyc.comb` 的 operands/region args/yield/results 与 storage 约束，编写 plan pass 的正反例测试。
2. 实现 `pyc-plan-comb-inputs`，在 IR 上生成直接输入、结果与 guardable 属性；将不安全路径显式标为无条件执行。
3. 扩展 C++ placement，使 per-comb snapshot 输入和跨调用结果持久保存，并为违反契约的 IR 发出诊断。
4. 在 emitter 中生成 `CombCache_N`、输入比较、输出 changed 标记、开关和统计；不修改 Wire 类和 primitive runtime。
5. 添加 IR、emitter、C++/Verilog 逐周期等价性及开关回退测试；先运行最小 C++ 编译/执行 gate。
6. 扩展性能脚本，采集 RegisterFile、DavinciOO 和两级层次流水线 example 的优化前后基线与 comb skip 统计。
7. 运行 G0/G1/G2 相关 gate、记录日志；依据结果更新 `docs/simulation.md` 和本文件的“实际结果/偏差”小节。



## 待确认事项

1. 输入 snapshot 与输出 changed metadata 不需要改动 RFC 0103–0109；未来实例级 event scheduler 可复用该 per-comb guard。是否接受此分阶段边界？
2. 性能门槛建议采用“低活动率至少 +10%、全活动率不超过 3% 退化”。若目标平台或 DavinciOO workload 有既定 SLA，应以其替换。
3. DavinciOO 的具体集成 workload 需要在实施前选定并固定输入 trace；建议选择现有带 valid/ready backpressure 的 `srcs/tests` 用例，而不把其独立 `model/` 事件队列作为后端设计来源。

