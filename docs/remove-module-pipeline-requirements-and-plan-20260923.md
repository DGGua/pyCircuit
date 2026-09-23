# 完全删除 Module Pipeline 功能需求分析与实施规划

日期：2026-09-23
目标分支：`feat/change-driven-scheduler`
基线：`a1ac8032`
状态：已批准并实施；module-pipeline 删除验证通过，另有一项实施前已存在的
schedule 诊断文本 gate 失败

## 背景与目标

`module-pipeline` 是当前分支新增的可选实验功能，用于分析 whole-instance 粗粒度
SCC，并在受限场景将 false SCC 重写成 stage functions。用户要求从该分支完全删除
此功能。

“完全删除”定义为：

- 删除 Pass 实现和公共 metadata 合同；
- 删除 `pycc`/`pyc-opt` 注册与 `--module-pipeline` CLI；
- 删除专用测试脚本和只服务于该功能的 fixture；
- 删除 `pyc.module_pipeline.*` 与 `pyc.pipeline.*` 的生产/消费逻辑；
- 删除 tracked 文档中把该功能描述为现有能力的内容；
- 不保留 deprecated、ignored 或 hidden compatibility option。

change-driven schedule、canonical CombDepGraph 和 instance-aware comb-cycle gate 必须
保留。删除 module-pipeline 后，默认 pipeline 继续使用 canonical cycle gate，
不能退回把 `pyc.instance` 当组合 cut point 的旧行为。

## 项目现状与执行流程定位

当前相关流程为：

```text
pycc --module-pipeline=off|analyze|rewrite
  → analyze/rewrite 时先 check clock domains + pack i1 regs
  → ModulePipelinePass
       → whole-instance coarse SCC
       → port-level dependency classification
       → 可选 stage func / instance rewrite
       → pyc.module_pipeline.v1 metadata
  → CheckCombCyclesPass
  → fuse comb
  → PlanChangeDrivenSchedulePass
  → CheckChangeDrivenSchedulePass
```

默认模式已经是 `off`。在 off 路径中，`CheckCombCyclesPass` 已使用 canonical
instance-aware graph，能够：

- 接受端口级无环的 whole-instance false SCC；
- 拒绝真实跨实例组合环；
- 继续为 change schedule 提供合法 DAG。

因此删除实验 pass 不要求恢复或新建另一套组合环语义。

## 相关现有实现说明

### Pass 与 metadata 合同

- `compiler/mlir/lib/Transforms/ModulePipelinePass.cpp`
  - analysis、rewrite、metadata 校验和错误码 `PYC4001`–`PYC4005`。
- `compiler/mlir/include/pyc/Transforms/ModulePipeline.h`
  - schema `pyc.module_pipeline.v1`；
  - module attr `pyc.module_pipeline.summary`；
  - function attrs `pyc.pipeline.validated/generated/origin/stage/logical_path/stage_dag`；
  - summary/error 类型和查询 API。
- `compiler/mlir/include/pyc/Transforms/Passes.h`
  - `createModulePipelinePass(bool rewrite = false)`。

### 构建与工具接线

- `compiler/mlir/CMakeLists.txt`
  - 编译 `ModulePipelinePass.cpp`。
- `compiler/mlir/tools/pyc-opt.cpp`
  - force-link pass。
- `compiler/mlir/tools/pycc.cpp`
  - 注册和解析 `--module-pipeline`；
  - 根据 mode 选择 pass pipeline；
  - `collectFrontendModuleSymbols()` 跳过 `pyc.pipeline.generated=true` 的 stage func。

### 测试

专用文件：

- `compiler/mlir/test/module_pipeline_smoke.sh`
- `compiler/mlir/test/Inputs/module_pipeline_false_scc.mlir`
- `compiler/mlir/test/Inputs/module_pipeline_true_cycle.mlir`
- `compiler/mlir/test/Inputs/module_pipeline_state_cut.mlir`
- `compiler/mlir/test/Inputs/module_pipeline_unsupported_multicall.mlir`

其中 false-SCC 和 true-cycle fixture 同时具有 canonical instance-aware cycle gate
价值。实施时将它们改名并迁移到 `change_schedule_gate.sh`，而不是随实验功能一起
丢掉跨实例合法性覆盖。

## 需求与验收标准

### 功能需求

1. 删除 module-pipeline Pass、header、factory、CMake 和 tool registration。
2. 删除 `--module-pipeline`；旧参数必须由 pycc 解析器拒绝。
3. pycc 固定走当前 off 分支的标准 gate 顺序：

   ```text
   CheckCombCycles
   → CheckClockDomains
   → PackI1Regs
   ```

4. `collectFrontendModuleSymbols()` 不再识别或忽略 `pyc.pipeline.generated`。
5. 删除专用 rewrite/metadata 测试。
6. 保留并改名两个通用 fixture：
   - false SCC：默认 pycc 必须接受；
   - true cross-instance cycle：默认 pycc 必须拒绝且诊断包含层次路径。
7. change schedule planner/verifier、dirty-only emitter 和 runtime 行为保持不变。
8. tracked 代码和现状文档中不再存在 module-pipeline schema、attrs、Pass 或 CLI。

### 非功能需求

- 不修改 CombDepGraph 的 instance-aware 语义。
- 不删除 `PlanChangeDrivenSchedulePass` / `CheckChangeDrivenSchedulePass`。
- 不改变 `comb/tick/commit/step`。
- 不触碰任务开始前的无关未跟踪文件。
- 这是 hard break，不提供 compatibility alias。

### 验收标准

- 搜索 `ModulePipeline`、`module-pipeline`、`module_pipeline`、
  `pyc.module_pipeline`、`pyc.pipeline.` 时，仅允许删除规划中的历史说明；
  生产代码和现状文档零残留。
- pycc 与 runtime test target 构建通过。
- `pycc --help` 不含旧 option，传旧 option 失败。
- false cross-instance SCC 通过，真实 cross-instance cycle 失败。
- change schedule、dirty scheduler、instance vector cache 和 primitive change
  reporting gates 通过。
- 既有 `change_schedule_gate.sh` tampered fixture 诊断文本问题单独记录，不借本次
  删除改变 verifier 诊断顺序。

## 方案设计

### 模块边界、输入与输出

删除前：

```text
CombDepGraph → ModulePipelinePass（可选）→ rewritten IR/metadata
             → Change Schedule
```

删除后：

```text
CombDepGraph → instance-aware CheckCombCycles
             → Change Schedule
```

输入仍是 frontend `.pyc`，输出仍是 C++/Verilog。唯一移除的是实验性的模块
stage-DAG analysis/rewrite 层。

### 上下游关系与数据/控制流

`CheckCombCyclesPass` 在提交 `d6797dfc` 后已经复用 canonical graph，并穿过 instance
边界。`PlanChangeDrivenSchedulePass` 随后同样使用 `FunctionCombDepGraph` 构建
node/rank/slot/fanout。因此 module-pipeline 不是 dirty schedule metadata 的必要
上游。

删除后：

- 真实跨实例环继续在 change schedule 之前失败；
- false whole-instance SCC 因端口级图无环而继续通过；
- 不再生成 stage func，不再重写 instance；
- 不再产生或接受 module-pipeline metadata 合同；
- normal emitter 直接消费原模块上的 change schedule。

### 文件和接口改动清单

#### 删除

- `compiler/mlir/include/pyc/Transforms/ModulePipeline.h`
- `compiler/mlir/lib/Transforms/ModulePipelinePass.cpp`
- `compiler/mlir/test/module_pipeline_smoke.sh`
- `compiler/mlir/test/Inputs/module_pipeline_state_cut.mlir`
- `compiler/mlir/test/Inputs/module_pipeline_unsupported_multicall.mlir`

#### 改名并迁移测试职责

- `module_pipeline_false_scc.mlir`
  → `comb_cycle_cross_instance_false_scc.mlir`
- `module_pipeline_true_cycle.mlir`
  → `comb_cycle_cross_instance_true.mlir`
- `change_schedule_gate.sh`
  - 增加 false SCC 正例；
  - 增加 true cycle 负例；
  - 增加 `--module-pipeline` 已移除的 CLI hard-break 检查。

#### 修改

- `compiler/mlir/include/pyc/Transforms/Passes.h`
  - 删除 factory declaration。
- `compiler/mlir/CMakeLists.txt`
  - 删除源文件。
- `compiler/mlir/tools/pyc-opt.cpp`
  - 删除 force-link。
- `compiler/mlir/tools/pycc.cpp`
  - 删除 option、mode parsing 和 conditional pipeline；
  - 固定执行标准 cycle/clock/pack gates；
  - 删除 generated-stage symbol 特判。
- `CHANGELOG.md`
  - 记录删除实验功能和 CLI hard break。
- `docs/change-driven-scheduler-implementation-20260923.md`
  - 删除 module-pipeline 功能、流程、文件、测试和限制说明；
  - 明确 dirty schedule 直接依赖 canonical graph。
- `docs/change-driven-dirty-only-requirements-and-plan-20260923.md`
  - 删除把 module-pipeline mode 列为保留项及对应测试结果。
- `docs/module-fallback-timing-measurement-plan-20260922.md`
  - 保留历史性能事实，但把具体实验 Pass 名改为一般的模块级调度重构候选，避免把
    已删除功能描述为当前分支能力。

### 边界条件、错误处理与兼容性

- 旧 `.mlir` 中的 discardable `pyc.pipeline.*` attrs 不再具有编译器合同；
  本次不增加兼容清理 Pass。
- 旧命令 `--module-pipeline=*` 直接失败。
- 删除 rewrite 后不再支持 stage func 生成、idempotent rewrite 或 PYC400x
  module-pipeline diagnostics。
- generic canonical cross-instance cycle diagnostics继续保留。
- false SCC 是否通过只由 canonical port-level dependencies 决定。

## 与既有文档和约束的一致性检查

- `docs/updatePLAN.md`：CombDepGraph、comb-cycle 和 logic-depth gate 仍保留；
  删除 backend pipeline 实验不降低 MLIR legality。
- Decisions 0127/0128/0134/0135：instance-aware graph 和真实组合环非法合同由
  `CheckCombCyclesPass` 保持。
- dirty-only scheduler：change schedule 仍在 canonical cycle gate 后生成和验证。
- 不引入 backend-only 替代实现。

## 测试与验证计划

1. 静态零残留搜索。
2. 构建：

   ```bash
   cmake --build .pycircuit_out/toolchain/build --target pycc -j2
   cmake --build .pycircuit_out/toolchain/build \
     --target pyc4_primitive_change_reporting_test -j2
   ```

3. CLI hard break：
   - help 不含 `--module-pipeline`；
   - 传旧 option 失败。
4. canonical graph：
   - false cross-instance SCC 通过；
   - true cross-instance cycle 失败；
   - local reg feedback 通过；
   - local true cycle 失败。
5. 回归：

   ```bash
   tests/runtime/run_change_scheduler.sh
   compiler/mlir/test/change_schedule_gate.sh
   compiler/mlir/test/comb_dirty_scheduler_smoke.sh
   compiler/mlir/test/instance_vector_cache_smoke.sh
   .pycircuit_out/toolchain/build/runtime/cpp/pyc4_primitive_change_reporting_test
   ```

6. `git diff --check` 和文档 reader review。

## 实施步骤

1. 删除 Pass 实现/header，并移除 factory/build/tool 接线。
2. 简化 pycc pipeline 和 frontend module symbol 收集。
3. 删除专用 tests，迁移两个 canonical cycle fixtures。
4. 扩展 change schedule gate。
5. 更新 changelog 和三份 tracked 文档。
6. 构建并运行验证计划。
7. 检视 diff，只提交本任务文件。
8. 回写实际结果。

## 待确认事项

无待确认架构项。请求批准：完全删除 module-pipeline 的 Pass、schema、attrs、CLI、
rewrite、PYC400x diagnostics 和专用 tests；不保留 hidden/deprecated mode；将
false-SCC 和 true cross-instance cycle 两个通用 fixture 改名迁移到 canonical
change-schedule/cycle gate，保证删除实验功能不降低跨实例组合环覆盖。

## 实施与验证结果

实施日期：2026-09-23

### 已实施

- 删除 `ModulePipeline.h`、`ModulePipelinePass.cpp`、factory、CMake source 和
  pyc-opt force-link。
- 删除 pycc 的 `--module-pipeline`、mode parsing、conditional pipeline 和
  `pyc.pipeline.generated` symbol 特判。
- pycc 固定执行：

  ```text
  CheckCombCycles → CheckClockDomains → PackI1Regs
  ```

- 删除 module-pipeline smoke、state-cut 和 unsupported-multicall fixtures。
- 将 false-SCC/true-cycle fixtures 改名为通用 cross-instance cycle fixtures，
  并迁入 `change_schedule_gate.sh`。
- 增加旧 CLI 必须失败的 hard-break 检查。
- 更新 changelog、change-driven 实现说明、dirty-only 规划和历史 fallback 测量文档。

### 验证通过

```text
cmake --build .pycircuit_out/toolchain/build --target pycc -j2
tests/runtime/run_change_scheduler.sh
compiler/mlir/test/comb_dirty_scheduler_smoke.sh
compiler/mlir/test/instance_vector_cache_smoke.sh
cmake --build .pycircuit_out/toolchain/build \
  --target pyc4_primitive_change_reporting_test -j2
.pycircuit_out/toolchain/build/runtime/cpp/pyc4_primitive_change_reporting_test
git diff --check
```

`change_schedule_gate.sh` 在到达既有 tampered fixture 前已确认：

- local reg feedback 通过；
- local true cycle 被拒绝；
- cross-instance false SCC 通过；
- cross-instance true cycle 被拒绝，诊断包含 `a_input` 和 `b_input`；
- pycc help 不含 `--module-pipeline`；
- 传旧 option 被拒绝；
- canonical schedule 两次输出稳定。

### 已知既有失败

`change_schedule_gate.sh` 最终仍因 tampered fixture 的诊断文本期望返回 1：

```text
期望：schedule fanout size mismatch
实际：change-driven schedule summary does not match canonical graph
```

tampered IR 已被正确拒绝；该问题与 module-pipeline 删除无关。
