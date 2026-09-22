# C++ 模块 fallback 时间占比测量需求分析与实施规划

日期：2026-09-22  
PyCircuit 分支：`measure/module-fallback-stats`  
基线：`upstream/main` @ `27b226cd2dc87a30b4a55b714e04f50eb49d4547`  
状态：已批准。使用当前脏 DavinciBaseLine（`lys` @ `b848b236`），不修改其源码。PyCircuit 只增加默认可关闭的统计。

## 背景与目标

模块流水重构及“最少 stage”搜索实现复杂，目前缺少证据说明
`eval_fixpoint_fallback_path()`、SCC 收敛和重复 `eval_comb_pass()` 在真实
DavinciBaseLine trace 中占多少时间。

本阶段只增加可关闭的统计和计时，不修改调度顺序、cache 判定、phase API、状态
语义或生成电路。目标是回答：

1. 被测顶层 `eval()` 总时间中，topological path、fallback path、primitive/
   instance eval 和重复 comb pass 各占多少；
2. 每次 `eval()` 的 fallback 迭代次数分布；
3. 哪些层次模块实际进入 fallback，哪些模块已经单遍拓扑执行；
4. 若删除重复收敛工作，理论收益是否足以支持后续模块拆分。

## 项目现状与执行流程定位

当前 `CppEmitter.cpp` 为每个生成模块选择：

```text
hasFullTopo = true
  → eval() 单遍执行 fullOrdered

hasFullTopo = false
  → eval_comb_pass()
  → eval_fixpoint_fallback_path()
       primitive/instance eval
       eval_comb_pass()
       直到输入 cache 不再变化
```

现有 `_pyc_sim_stats_t` 只有：

- `instance_eval_calls`
- `instance_cache_skips`
- `primitive_eval_calls`
- `primitive_cache_skips`
- `fallback_iterations`

它没有 `eval_calls`、路径调用次数或纳秒计时；`dump_sim_stats()` 也只输出当前
对象，不递归子实例。Davinci runner 当前不会显式调用 dump API。

## 相关现有实现说明

### PyCircuit

- `compiler/mlir/lib/Emit/CppEmitter.cpp`
  - `_pyc_sim_stats_t` 与环境变量初始化；
  - `eval_comb_pass()`；
  - `eval_fixpoint_fallback_path()`；
  - `eval_scc_comp_*()` / `eval_fast_scc_path()`；
  - `eval()`；
  - instance member 和层次路径。
- `compiler/mlir/include/pyc/Emit/CppEmitter.h`
  - 本次预计不改公开 API。
- `runtime/cpp/pyc_tb.hpp`
  - 本次不改；仍执行沿前/沿后 `comb()`。

### DavinciBaseLine

现成流程：

```text
model/rtl/build_pycircuit_module.py
  → 生成 v5_superscalar_v5_core C++
model/rtl/replay_pycircuit_trace.py
  → 转换 PTO trace
  → 编译 pycircuit_trace_runner.cpp + 生成 C++
  → 输出 replay.json
```

候选 trace：

- `examples_beginner_hello_world.pto.trace`：确定性基线成熟，但既有记录显示长时间
  stall，可能夸大低活动状态；
- `models_qwen3_14b_decode_fwd.pto.trace`：历史 all-modular 性能分析使用过，
  更适合作为本次唯一主测 trace。

本方案推荐后者。

## 需求与验收标准

### 功能需求

1. 统计默认关闭；关闭时不调用时钟、不写文件。
2. `PYC_SIM_STATS=1` 保留现有计数。
3. 新增 `PYC_SIM_TIMING=1` 后才执行 `steady_clock` 计时。
4. `PYC_SIM_STATS_PATH=<path>` 由顶层对象析构时写一次统计文件，不要求修改
   Davinci runner。
5. 顶层 dump 递归列出子实例；每行带稳定层次路径。
6. 统计不得修改 cache valid、changed flag、求值顺序或收敛条件。
7. 同一输入下，统计开关前后的 `replay.json` 确定性字段完全一致。

### 统计字段

每个实例至少记录：

- `path`
- `eval_calls`
- `eval_total_ns`
- `topo_eval_calls`
- `topo_eval_ns`
- `fallback_calls`
- `fallback_total_ns`
- `initial_comb_pass_calls`
- `initial_comb_pass_ns`
- `fallback_comb_pass_calls`
- `fallback_comb_pass_ns`
- `fallback_primitive_ns`
- `fallback_iterations`
- `fallback_max_iterations`
- `instance_eval_calls`
- `instance_cache_skips`
- `primitive_eval_calls`
- `primitive_cache_skips`

迭代直方图至少包含 `0/1/2/3/4+`。统计采用 `uint64_t` 饱和前无需额外处理；
一次 trace 不会接近溢出。

### 报告指标

顶层总体比例：

```text
fallback_share = top.fallback_total_ns / top.eval_total_ns
fallback_comb_share = top.fallback_comb_pass_ns / top.eval_total_ns
initial_comb_share = top.initial_comb_pass_ns / top.eval_total_ns
```

模块明细：

```text
module_fallback_share = instance.fallback_total_ns / instance.eval_total_ns
avg_fallback_iterations = fallback_iterations / fallback_calls
```

子实例时间与父实例时间嵌套，不能直接求和。总体占比只使用顶层；子实例只用于
定位热点。

`fallback_comb_pass_ns` 是“fallback 循环中的 comb 工作”，不直接宣称全部可
删除。真正可删除比例必须等模块 DAG 实现后做 A/B；本阶段只报告事实时间。

### 非功能验收

- 统计关闭时，相同二进制运行 5 次的中位数相对当前基线退化不超过 1%；
- 统计开启时允许变慢，但 `cycles/fetch_complete/timed_out` 等确定性结果一致；
- 每个模块满足各子阶段时间不超过 `eval_total_ns` 的合理关系；父子嵌套除外；
- 统计文件可被脚本解析，字段缺失或除零明确报错；
- 重复运行 5 次，顶层 `fallback_share` 报告中位数、最小值、最大值。

## 方案设计

### 计时原则

使用 `std::chrono::steady_clock`，只在 `_pyc_sim_timing_enable` 为真时读取时钟。
不使用 CPU cycle counter，避免不同架构和频率缩放下不可比。

不在每个 `eval_comb_N()` 内计时：调用数量巨大，计时开销会主导结果。本次只在
路径边界计时：

- `eval()` 总入口/出口；
- topological body；
- initial `eval_comb_pass()`；
- 每轮 primitive/instance group；
- 每轮 fallback `eval_comb_pass()`；
- SCC fast path 总体。

### 文件和接口改动清单

#### `compiler/mlir/lib/Emit/CppEmitter.cpp`

1. 生成代码增加 `<chrono>` include。
2. 扩展 `_pyc_sim_stats_t` 上述计数和纳秒字段。
3. 增加 `_pyc_sim_timing_enable`，从 `PYC_SIM_TIMING` 读取。
4. 在生成的 `eval()` 中：
   - timing 开启时记录入口；
   - `hasFullTopo` 路径累加 `topo_eval_*`；
   - fallback 路径分别记录 initial comb 和 fallback 总时间；
   - 统一出口累加 `eval_total_ns`。
5. 在 `eval_fixpoint_fallback_path()` 中：
   - 记录每轮 primitive group 时间；
   - 记录每轮 `eval_comb_pass()` 时间；
   - 更新 iterations、max 和 histogram；
   - 不改变 break 条件。
6. 在 SCC fast path 中记录路径总时间和迭代直方图，便于
   `PYC_SIM_FAST=0/1` 对照。
7. 将 `dump_sim_stats()` 扩展为稳定 TSV/JSONL；推荐 JSONL，每个实例一行。
8. 新增 `dump_sim_stats_tree(os, prefix)`：
   - 先输出当前实例；
   - 按稳定排序递归调用 child；
   - prefix 使用现有 instance member/name，不引入新的 trace path 语义。
9. 仅顶层 `func.func` 生成析构函数：
   - 若 `PYC_SIM_STATS_PATH` 非空，析构 body 中打开文件并调用 tree dump；
   - 析构 body 执行时 child member 尚未析构；
   - 文件只写一次，避免子模块覆盖同一路径。
10. 保留现有 `dump_sim_stats(std::ostream&)` 和
    `dump_sim_stats_to_path()` 兼容入口。

#### `flows/tools/perf/analyze_module_fallback_stats.py`（新增）

输入 1–N 个统计文件，输出 JSON 与 Markdown：

- 校验 schema/字段；
- 顶层各时间占比；
- fallback share 最高的模块实例；
- fallback iterations 最高的模块实例；
- 5 次运行的 median/min/max；
- 明确标注父子时间不能相加。

#### `compiler/mlir/test/sim_timing_stats_smoke.sh`（新增）

构造两个最小输入：

- full-topo 模块：`topo_eval_calls>0`、`fallback_calls=0`；
- 会进入现有 fallback 的层次模块：`fallback_calls>0`、时间字段非零。

分别运行 stats off/on，比较功能输出，并检查 JSONL 可解析。

#### `flows/scripts/run_sim_timing_stats_gate.sh`（新增）

- 构建 pycc；
- 运行 smoke；
- 调 parser；
- 写 `docs/gates/logs/<run-id>/commands.txt`、stdout/stderr 和 summary。

#### 明确不修改

- `CheckCombCyclesPass.cpp`
- `CombDepGraph.{h,cpp}`
- `PYCOps.td/.cpp`
- `runtime/cpp/pyc_tb.hpp` 及 include 镜像
- `compiler/frontend/pycircuit/cli.py`
- DavinciBaseLine 源码和 runner

### Davinci trace 测量流程

主测 trace：

`model/tests/fixtures/traces/models_qwen3_14b_decode_fwd.pto.trace`

产物全部放 `/tmp/pyc-module-fallback-timing-20260922/`。

流程：

1. 用本分支构建并安装 pycc。
2. 从选定的 DavinciBaseLine 工作树生成 `v5_superscalar_v5_core` C++。
3. 使用现有 `replay_pycircuit_trace.py` 编译 runner。
4. stats 关闭运行一次，记录 `replay.json` 和墙钟基线。
5. stats/timing 开启运行一次预热，不纳入结果。
6. stats/timing 开启运行 5 次，每次使用独立统计路径和 replay workspace。
7. parser 汇总时间占比。
8. 可用时补充一次 `perf record/report` 作为旁证；perf 不作为必需验收。

建议环境：

```bash
export PYCIRCUIT_ROOT=/home/lidongzhe/pyCircuit
export PYC_SIM_STATS=1
export PYC_SIM_TIMING=1
export PYC_SIM_STATS_PATH=/tmp/pyc-module-fallback-timing-20260922/stats-1.jsonl
export PYC_REPLAY_OPT='-O3'
```

具体生成和 replay 参数以所选 DavinciBaseLine 工作树现有
`run_pycircuit_trace_align.sh` / 对齐文档为准；不复制 runner 或手写 PTO flow。

### 边界条件与风险

1. Davinci replay 当前编译参数包含 `-DPYC_DISABLE_INSTANCE_EVAL_CACHE`。这会
   显著影响 fallback 收敛和时间占比；测量必须把该宏写入报告。如果生成模型因
   此在 fallback 上不收敛，停止并报告，不在 PyCircuit 中偷偷改变 cache 语义。
2. 当前 DavinciBaseLine 工作树存在大量未提交修改。不得在其中写源文件；
   需要用户确认使用当前树还是干净 worktree。
3. `steady_clock` 有测量开销。通过“路径级而非 comb 级计时”控制开销，并报告
   timing on/off 墙钟差。
4. 如果主测模型已经全部走 full-topo，fallback share 为 0 也是有效结论，说明
   当前模块拆分方向没有实际收益。
5. 如果 trace 超时，保留转换/生成/编译和部分日志，不改用更短 trace 后冒充
   同一测量。

## 与既有文档和约束的一致性检查

- `docs/updatePLAN.md`：本阶段只测量，不改变语义；为后续 gate-first 决策提供
  基线。
- Decisions 0127/0128/0134：不改变组合环判定。
- Decisions 0026/0027/0113：不改变 comb/tick/commit 和 observation point。
- 既有 `comb-output-activity` 报告测量输出变化率，本方案测量调度路径墙钟占比，
  两者互补，不能互相替代。
- DavinciBaseLine `model/` 信息只用于本地测试，报告不复制机密微结构参数。

## 测试与验证计划

1. 编译 gate：pycc 和 generated C++ 能编译。
2. full-topo smoke：fallback 为零。
3. fallback smoke：fallback 计数、时间和 histogram 非零。
4. stats off/on 功能输出一致。
5. parser 单文件和五文件聚合正确。
6. Davinci full trace 完成并生成 5 份可解析统计。
7. 报告记录 PyCircuit SHA、Davinci SHA/dirty 状态、trace、编译器、flags、
   主机信息和全部复现命令。

## 实施步骤

1. 用户确认 DavinciBaseLine 基线后，更新本文。
2. 扩展 emitter 统计和递归 dump。
3. 新增 parser、smoke 和 gate。
4. 构建 pycc，先跑最小 smoke。
5. 生成 Davinci C++ 并跑主测 trace。
6. 汇总 5 次占比，写回本文“实测结果”。
7. 根据 fallback share 决定：
   - `<5%`：停止模块拆分优化；
   - `5%–15%`：只考虑简单单模块拆分原型；
   - `>15%`：再评估完整 module-pipeline pass。

## 待确认事项

### 已决定 1：使用当前脏 DavinciBaseLine

用户已批准选项 A。测量读取 `/home/lidongzhe/DavinciBaseLine`，不在其中写入源文件。报告记录 HEAD、dirty 状态和 diff hash。该结果代表本地开发树，不代表远端 `origin/lys`。

### 待确认 1：DavinciBaseLine 使用当前工作树还是干净基线

**为何现在必须决定：**

`/home/lidongzhe/DavinciBaseLine` 当前位于 `lys`，有大量 tracked 修改和未跟踪
文件，包括生成、runner、replay、参数和 v5 核实现。它们会直接改变生成模型、
编译参数和 trace 运行结果。测量若不固定基线将无法复现。

**相关背景：**

本阶段不需要修改 DavinciBaseLine 源码。当前工作树包含最近的 v5 replay 修复，
更可能直接跑通；干净 `origin/lys` 可复现，但可能缺少这些尚未提交的修复。

**选项与结果：**

- **选项 A：使用当前脏工作树**
  - 不修改其中任何源文件，只读取并运行；
  - 报告记录 HEAD、完整 dirty 文件清单和 diff hash；
  - 测量代表当前本地开发状态，不代表远端 `origin/lys`。
- **选项 B：创建临时干净 worktree 检出 `origin/lys`**
  - 测量基线可复现，不受本地修改影响；
  - 若缺少当前工作树中的生成/replay 修复，trace 可能无法跑通，需要停止或另行
    批准移植最小修复；
  - 测量结束删除临时 worktree。

**推荐：**

选项 A。用户目标是判断当前准备优化的真实本地流程，现有 replay 修复很可能是
跑通 v5 trace 的必要条件；只要报告明确 dirty 状态即可。
