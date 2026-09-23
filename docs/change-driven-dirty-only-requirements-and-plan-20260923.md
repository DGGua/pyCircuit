# Change-Driven Scheduler 固化 Dirty 模式需求分析与实施规划

日期：2026-09-23
目标分支：`feat/change-driven-scheduler`
基线：`7d84e293`
状态：已批准并实施；dirty-only 验证通过，另有一项实施前已存在的 schedule
诊断文本 gate 失败

## 背景与目标

当前 C++ emitter 同时保留 `always`、`guarded`、`dirty` 三种 fused-comb 更新模式，
并由 `pycc --comb-update` 和 Python build CLI 进行选择。用户最新要求是不再提供
模式选择：change-driven scheduler 始终采用 dirty 行为，任何调用路径都不得切换
到 always 或 guarded。

本次目标不是把默认值从 dirty 改成另一个默认值，而是执行一次 hard break：

- 删除 `--comb-update` CLI；
- 删除 emitter 的 `CombUpdateMode` enum 和 option field；
- 删除 always/guarded 生成分支；
- 含 fused comb 的 C++ 生成固定采用 dirty guard；所有适用输出固定采用语义发布
  和 direct fanout；
- 更新测试、文档和已失效的 mode A/B 工具；
- 旧命令若继续传 `--comb-update=...`，由命令行解析直接报告 unknown option，
  不静默忽略。

## 项目现状与执行流程定位

当前选择链路为：

```text
python -m pycircuit.cli build --comb-update=<mode>
  → cli.py 将 mode 写入 build flags/cache key
  → 调用 pycc --comb-update=<mode>
  → pycc 解析 always|guarded|dirty
  → 写入 CppEmitterOptions::combUpdateMode
  → CppEmitter.cpp 在 comb、instance、primitive、state、return 路径分支
```

默认值虽然已经是 dirty，但 always 和 guarded 仍会改变生成类的数据成员和执行行为：

- always 不生成 comb snapshots/dirty set，直接执行和发布；
- guarded polling 所有 comb inputs，但不做 metadata fanout；
- dirty 只 polling 非 schedule boundary，并按语义变化传播本地 comb fanout。

测试脚本当前用三模式矩阵进行 C++ 结果对照，并比较三种模式的 Verilog 文本一致。

## 相关现有实现说明

### C++ emitter 公共配置

`compiler/mlir/include/pyc/Emit/CppEmitter.h` 中：

- `CppEmitterOptions::CombUpdateMode` 定义 `Always/Guarded/Dirty`；
- `combUpdateMode` 默认 `Dirty`；
- emitter 的多个 helper 接收 mode 后选择分支。

外部代码没有独立依赖该 enum；仓库内只有 `pycc.cpp` 设置该字段。

### pycc 和 frontend CLI

`compiler/mlir/tools/pycc.cpp`：

- 注册 `--comb-update`；
- 解析并校验三个字符串；
- 单文件和 split-module 两条 C++ emission 路径都设置 emitter option。

`compiler/frontend/pycircuit/cli.py`：

- build parser 暴露 `--comb-update`；
- `_cmd_build()` 将值加入 pycc 参数和 build manifest/cache flags。

### 测试和工具

直接引用模式的文件包括：

- `compiler/mlir/test/comb_dirty_scheduler_smoke.sh`
- `compiler/mlir/test/Inputs/comb_dirty_scheduler_driver.cpp`
- `compiler/mlir/test/instance_vector_cache_smoke.sh`
- `compiler/mlir/test/module_pipeline_smoke.sh`
- `flows/tools/perf/run_change_driven_ab.py`
- `flows/tools/perf/test_change_driven_perf_tools.py`
- `docs/change-driven-scheduler-implementation-20260923.md`

其中 performance runner 的 variant 固定命名为
`current/dag-always/dag-dirty`。hard break 后 `dag-always` 不再是同一二进制的
合法配置，因此该工具和单测失去原有合同。

## 需求与验收标准

### 功能需求

1. `pycc --help` 和 Python build `--help` 均不再显示 `--comb-update`。
2. 向任一 CLI 传 `--comb-update` 必须失败，不允许接受后忽略。
3. `CppEmitterOptions` 不再暴露 `CombUpdateMode` 或 `combUpdateMode`。
4. 含 fused comb 的 C++ emission 固定生成 dirty snapshots 和 `DirtyBitset`；
   所有适用输出固定使用语义结果比较和 metadata direct fanout。
5. instance、primitive、state tick/commit 和顶层 return 固定采用原 dirty 分支。
6. Verilog emission 不受影响，也不再接收无意义的 `--comb-update`。
7. 删除或改写所有仓库内调用，确保没有残留 mode switch。

### 兼容性要求

- 这是 pyc4.0 hard-break，不保留 deprecated alias。
- 旧构建脚本必须删除 `--comb-update=dirty`；旧 always/guarded 调试命令不再可用。
- C++ `always` 不再作为同一版本 reference path。正确性改由固定期望、状态/层次/
  primitive smoke 和 C++/Verilog 对照承担。
- 不改 `comb/tick/commit/step` API，不改变 dirty 模式当前语义。

### 验收标准

- 实现代码和正常调用路径中不存在 `CombUpdateMode`、`combUpdateMode` 或
  `PYC_EXPECT_COMB_MODE`；`comb-update` 只允许出现在 hard-break 负向测试和迁移说明。
- 生成的有 comb C++ 包含 `DirtyBitset`、`_pyc_mark_comb_dirty` 和 direct fanout。
- 生成 C++ 不含运行时模式分支。
- pycc 构建通过。
- runtime scheduler、dirty scheduler、module pipeline、instance vector cache 和
  schedule gate 按更新后的预期运行。
- `docs/change-driven-scheduler-implementation-20260923.md` 只描述 dirty-only
  现状，不再建议模式切换。

## 方案设计

### 模块边界、输入与输出

本次不改变 MLIR schedule schema、CombDepGraph、module-pipeline 或 runtime
`DirtyBitset` 接口。变化边界位于 C++ emission policy：

```text
输入：已通过 pyc-check-change-driven-schedule 的 MLIR
输出：唯一 dirty 策略的 C++ 模型
```

Verilog emitter 不读取 C++ 更新策略，保持原行为。

### 上下游关系与数据/控制流

修改后流程为：

```text
Python build / pycc
  → 不再解析 comb mode
  → MLIR planner/verifier 生成并校验 change schedule
  → CppEmitter 无条件读取 metadata
  → 有 fused comb 时生成 dirty snapshots + DirtyBitset
  → 适用输出固定使用 semantic publish
  → runtime 只执行 dirty 行为
```

### 文件和接口改动清单

#### `compiler/mlir/include/pyc/Emit/CppEmitter.h`

- 删除 `CombUpdateMode`；
- 删除 `combUpdateMode`；
- 将注释改为 C++ emitter 固定使用 change-driven dirty 更新。

#### `compiler/mlir/lib/Emit/CppEmitter.cpp`

- `emitCombInputGuard()` 删除 mode 参数，保留 dirty 实现；
- `emitCombResultPublish()` 删除 mode 参数，固定为 compare + direct fanout；
- 无条件为 comb 生成 snapshots 和 dirty set；
- 构造函数无条件将全部本地 comb 初始标 dirty；
- eval node 无条件使用 dirty active guard；
- instance 输出无条件按语义变化发布；
- primitive output snapshots/wake 无条件生成；
- 顶层 return 固定 compare-before-store；
- state `tick_compute()` 固定使用 input snapshot guard；
- state `tick_commit()` 固定按 changed output direct wake；
- 删除所有 mode 条件和 mode 参数传递。

#### `compiler/mlir/tools/pycc.cpp`

- 删除 `--comb-update` option；
- 删除字符串解析和错误分支；
- 两条 emission 路径不再设置 emitter mode。

#### `compiler/frontend/pycircuit/cli.py`

- 删除 build parser 的 `--comb-update`；
- 删除 `args.comb_update`；
- 删除传给 pycc 的 flag；
- 从 build flags/cache manifest 删除 `comb_update`。

#### 测试

- `compiler/mlir/test/comb_dirty_scheduler_smoke.sh`
  - 删除 CLI option existence 检查；
  - 删除 always/guarded/dirty 循环；
  - 只生成一次 dirty-only C++ 和 Verilog；
  - 保留 first eval、inactive skip、semantic publish、reconvergence、state、
    instance、primitive 和 stats-removal 检查；
  - 增加 pycc/frontend 传旧 flag 必须失败的 hard-break 检查。
- `compiler/mlir/test/Inputs/comb_dirty_scheduler_driver.cpp`
  - 删除 `PYC_EXPECT_COMB_MODE` 宏和模式输出；
  - 保留功能断言。
- `compiler/mlir/test/instance_vector_cache_smoke.sh`
  - 删除显式 `--comb-update=dirty`。
- `compiler/mlir/test/module_pipeline_smoke.sh`
  - 删除所有 `--comb-update`；
  - 原 guarded 组合改为唯一 dirty 路径。

#### 性能工具

- 删除 `flows/tools/perf/run_change_driven_ab.py` 和
  `flows/tools/perf/test_change_driven_perf_tools.py`。

理由：该工具的固定合同就是同一实现内比较 `dag-always/dag-dirty`。dirty-only
之后该比较不再成立。未来若比较旧 commit 与 dirty-only 新 commit，应新建
`baseline/candidate` 二进制级 benchmark 工具，不能重新引入 runtime mode。

#### 文档

- 更新 `CHANGELOG.md`，记录 `--comb-update` hard break 和 dirty-only。
- 更新 `docs/change-driven-scheduler-implementation-20260923.md`：
  - 删除三模式功能和 A/B mode runner；
  - 将执行流程改为唯一 dirty 路径；
  - 验证结果改为 dirty-only gate。

### 边界条件、错误处理与兼容性

- 没有 `pyc.comb` 的函数不生成零容量 `DirtyBitset`。
- 有 boundary input 的 comb 继续 snapshot/polling；纯 schedule-driven comb
  只由 dirty bit 激活。
- 第一次构造仍将所有本地 comb 标 dirty，保证首轮完整求值。
- instance/primitive cache 宏仍保留；它们控制 cache 实现，不是 comb update mode。
- `PYC_SIM_FAST`/SCC fallback 选择不属于 `--comb-update`，本次不删除。
- module-pipeline `off|analyze|rewrite` 是编译期层次处理选择，不属于 comb update，
  本次不修改。

## 与既有文档和约束的一致性检查

- `docs/updatePLAN.md`：dirty schedule 仍在 MLIR planner/verifier gate 后由 emitter
  消费，符合 gate-first。
- Decision 0108/0109：本次不宣称已经完成全图 event queue，只删除旧参考策略。
- Decision 0129：当前仍是二值 `Wire` 变化检测；dirty-only 不扩大该语义。
- pyc4.0 hard-break 允许移除旧 CLI，不保留兼容 alias。
- 不引入 backend-only 新语义；只是把已经由统一 metadata 驱动的 dirty 策略固化为
  唯一 C++ 执行策略。

## 测试与验证计划

1. 静态搜索确认 mode symbol 和 CLI flag 零残留。
2. 构建：

   ```bash
   cmake --build .pycircuit_out/toolchain/build --target pycc -j2
   ```

3. CLI hard-break：
   - `pycc --help` 不含 `--comb-update`；
   - `pycc ... --comb-update=dirty` 失败；
   - frontend build help 不含该参数；
   - frontend 传该参数失败。
4. runtime：

   ```bash
   tests/runtime/run_change_scheduler.sh
   ```

5. emitter 和 integration：

   ```bash
   compiler/mlir/test/comb_dirty_scheduler_smoke.sh
   compiler/mlir/test/module_pipeline_smoke.sh
   compiler/mlir/test/instance_vector_cache_smoke.sh
   compiler/mlir/test/change_schedule_gate.sh
   ```

6. 生成物检查：
   - dirty symbols 存在；
   - always/guarded mode symbol 不存在；
   - C++ 编译运行；
   - Verilog 仍可生成。
7. 更新实现说明文档后做一次无上下文 reader review。

`change_schedule_gate.sh` 当前另有已知诊断文本顺序问题；实施中只在本次 emitter/
CLI 修改导致新失败时修复，不借本任务改变 verifier 诊断优先级。最终结果会分别
报告“本次通过项”和“既有已知失败”。

## 实施步骤

1. 删除公共 option 和 CLI 参数。
2. 把 emitter 的 dirty 分支内联为唯一路径，逐段消除 mode 条件。
3. 更新所有仓库调用方和测试。
4. 删除失效的三 variant A/B 工具。
5. 更新 changelog 和实现说明文档。
6. 构建并运行验证计划。
7. 检视 diff，确认未触碰任务开始前的未跟踪文件
   `runtime/cpp/bench_naive_mt.cpp` 及其他无关文档。
8. 将实际验证结果回写本文档。

## 待确认事项

无待确认架构项。请求批准以下 hard-break 方案：彻底删除 `--comb-update` 和
`CombUpdateMode`，不保留 deprecated/ignored alias；C++ emitter 永远生成当前
dirty 路径；删除 always/guarded 测试矩阵和失效的三 variant A/B 工具；保留
instance/primitive cache 开关、`PYC_SIM_FAST` 和 module-pipeline mode，因为它们
不是 comb update 策略选择。

## 实施与验证结果

实施日期：2026-09-23

### 已实施

- 删除 `CppEmitterOptions::CombUpdateMode` 和 `combUpdateMode`。
- 删除 pycc 与 Python build CLI 的 `--comb-update`。
- 删除 build manifest/profile 中的 `comb_update` 字段。
- 将 comb guard、semantic publish、instance output、primitive output、state
  tick/commit 和 top return 全部固化为原 dirty 路径。
- 更新 scheduler、module-pipeline 和 vector-cache smoke，移除正常路径中的旧参数。
- scheduler smoke 新增 pycc/frontend 旧参数必须失败的 hard-break 检查。
- 删除 `run_change_driven_ab.py` 及其三模式单测。
- 更新 `CHANGELOG.md` 和 change-driven 实现说明。
- 未修改任务开始前存在的 `runtime/cpp/bench_naive_mt.cpp` 及其他无关未跟踪文件。

### 验证通过

```text
cmake --build .pycircuit_out/toolchain/build --target pycc -j2
tests/runtime/run_change_scheduler.sh
compiler/mlir/test/comb_dirty_scheduler_smoke.sh
compiler/mlir/test/module_pipeline_smoke.sh
compiler/mlir/test/instance_vector_cache_smoke.sh
cmake --build .pycircuit_out/toolchain/build \
  --target pyc4_primitive_change_reporting_test -j2
.pycircuit_out/toolchain/build/runtime/cpp/pyc4_primitive_change_reporting_test
python3 -m py_compile compiler/frontend/pycircuit/cli.py
git diff --check
```

`comb_dirty_scheduler_smoke.sh` 同时确认：

- 两个 CLI 的 help 均无旧参数；
- 两个 CLI 传旧参数均失败；
- 生成 C++ 含 dirty set、canonical schedule 和 direct fanout；
- first eval、idle、semantic publish、reconvergence、state、instance 和 primitive
  行为正确；
- 生成 C++ 无 simulator statistics 残留。

### 已知既有失败

`compiler/mlir/test/change_schedule_gate.sh` 仍返回 1。其正向 schedule/cycle 检查
通过，tampered IR 也被拒绝；失败仅因 fixture 期望
`schedule fanout size mismatch`，而 verifier 先报告
`change-driven schedule summary does not match canonical graph`。该问题在本次
dirty-only 修改前已存在，未扩大范围改变 verifier 诊断顺序。
