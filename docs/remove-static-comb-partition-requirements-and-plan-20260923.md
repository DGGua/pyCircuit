# 完全删除 Static Comb Partition 功能需求分析与实施规划

日期：2026-09-23
目标分支：`feat/change-driven-scheduler`
基线：`716a01f2`
状态：已批准并实施；partition 删除验证通过，另有一项实施前已存在的 schedule
诊断文本 gate 失败

## 背景与目标

当前分支混入了可选的 static comb partition/supernode 功能。它通过
`--comb-partition=static` 把组合逻辑切成带 `pyc.partition.*` metadata 的 sibling
`pyc.comb`，默认模式为 `none`。用户要求删除该功能及相关代码，同时明确保留从
其他分支合入的 PCH 功能。

本次“完全删除”包括：

- 删除 partition Pass、verifier、公共 metadata header；
- 删除 pycc/pyc-opt/CMake/Passes 接线；
- 删除 `--comb-partition`、`--comb-partition-max-nodes`；
- 删除 `pyc.partition.*` 和 `pyc.comb.result_names` partition 合同；
- 删除 supernode/partition 专用测试、fixture 和检查脚本；
- 更新 change-driven 现状文档。

本次明确不删除：

- C++ member placement；
- C++ PCH 和 compile budget；
- `CombMemoization`、`CheckCombMemoizablePass`；
- canonical `CombDepGraph`、cycle gate；
- `FuseCombPass` 和 dirty-only change schedule。

## 项目现状与执行流程定位

当前 pipeline 中 partition 相关流程为：

```text
CheckFrontendContract
  → CheckCombPartitions（拒绝输入中的坏 metadata）
  → CheckCombMemoizable
  → lowering / cleanup
  → FuseComb（仅 partition 未启用）
  → CheckCombMemoizable
  → 可选 PartitionCombPass
  → CheckCombMemoizable
  → CheckCombPartitions
  → CheckCombCycles
  → Plan/CheckChangeDrivenSchedule
```

CLI：

```text
--comb-partition=none|static
--comb-partition-max-nodes=<N>
```

默认 `none`。change schedule 的 builder 和 C++ emitter 均不读取 partition attrs，
只读取 canonical `FunctionCombDepGraph` 和最终 `pyc.comb` 结果。因此 static
partition 不是 dirty scheduler 的必要输入。

## 相关现有实现说明

### Partition 实现

- `compiler/mlir/lib/Transforms/PartitionCombPass.cpp`
  - 建立 partition plan；
  - 重写 `pyc.comb`；
  - 写入 function/per-comb `pyc.partition.*`；
  - 使用 `pyc.comb.result_names` 保存 promoted result names。
- `compiler/mlir/lib/Transforms/CheckCombPartitionsPass.cpp`
  - 校验 metadata 类型、完整性、part 顺序、work、boundary 和命名。
- `compiler/mlir/include/pyc/Transforms/CombPartition.h`
  - 定义 schema attr names 和 `gsim-unified-v2`。

### 必须保留的 memoization 合同

`CombMemoization.h/.cpp` 和 `CheckCombMemoizablePass.cpp` 与 partition 同批进入分支，
但职责不同：

- 给 fused `pyc.comb` 提供确定性 op/type whitelist；
- 拒绝不能安全缓存或比较的 comb body；
- `FuseCombPass` 复用同一 whitelist；
- dirty semantic publish 依赖该合同。

因此删除 partition 时不回退 memoization gate。

### 测试

partition 专用测试：

- `compiler/mlir/test/supernode_partition_gate.sh`
- `compiler/mlir/test/check_supernode_partition.py`
- `compiler/mlir/test/check_comb_partition_contract.py`
- `compiler/mlir/test/Inputs/supernode_invalid_partition.mlir`
- `compiler/mlir/test/Inputs/supernode_optional_update.py`
- `compiler/mlir/test/Inputs/supernode_unused_livein.mlir`

`supernode_nonmemoizable.mlir` 同时覆盖独立的 memoization whitelist。实施时将它改名
为 `comb_nonmemoizable.mlir`，删除 supernode 注释，并把负例迁入
`change_schedule_gate.sh`，避免删除 partition 时丢失 dirty scheduler 合法性覆盖。

## 需求与验收标准

### 功能需求

1. 删除 `PartitionCombPass`、`CheckCombPartitionsPass` 和 `CombPartition.h`。
2. 删除相关 factory、CMake 和 pyc-opt registration。
3. 删除两个 pycc CLI option、解析、冲突检查和 conditional pipeline。
4. pycc 固定按现有非 partition 路径运行 `FuseCombPass`：

   ```text
   enableFuseComb = (!cppOnly) || !cppOnlyPreserveOps
   ```

5. 保留 pipeline 中 `CheckCombMemoizablePass`，可去除重复但不得删除 legality gate。
6. 删除全部 `pyc.partition.*`/`pyc.comb.result_names` 合同和专用 tests。
7. 旧 CLI option 必须被拒绝，不允许 ignored compatibility alias。
8. memoization 非法 fixture 迁入 change schedule gate 并继续失败。
9. dirty-only emitter、change schedule metadata 和 C++/Verilog 语义保持不变。

### 非功能需求

- PCH、member placement、compile budget 完全不动。
- 不修改 CombDepGraph 的 node/edge/cut-point 定义。
- 不修改 `PlanChangeDrivenSchedulePass`/`CheckChangeDrivenSchedulePass`。
- 不触碰任务开始前的无关未跟踪文件。

### 验收标准

- 生产代码和当前现状文档中不存在：
  - `PartitionCombPass`
  - `CheckCombPartitionsPass`
  - `CombPartition`
  - `pyc.partition.*`
  - `pyc.comb.result_names`
- `comb-partition` 字符串只允许存在于 CLI hard-break 负向测试和删除记录。
- pycc help 不含两个旧 option，传入任一 option 失败。
- `comb_nonmemoizable.mlir` 被 `CheckCombMemoizablePass` 拒绝。
- change schedule、dirty emitter、runtime、instance vector cache 和 primitive
  change reporting 回归通过。
- PCH/member placement gates 和文件无修改。

## 方案设计

### 模块边界、输入与输出

删除前：

```text
普通/fused comb
  → 可选 static partition rewrite
  → partition metadata verifier
  → change schedule
```

删除后：

```text
FuseComb
  → CheckCombMemoizable
  → canonical cycle gate
  → change schedule
```

change schedule 仍以最终 `pyc.comb` result 为 schedule node。只是删除一种可选的
comb 重写来源。

### 上下游关系与数据/控制流

`FunctionCombDepGraph` 同时被 cycle gate 和 change schedule 使用。partition 仅调用
该图帮助自己重写 IR，图本身不依赖 partition attrs。删除 partition 后：

- 默认 fused comb 数据流不变；
- memoization whitelist 不变；
- canonical graph 不变；
- schedule node/rank/slot/fanout 仍生成并校验；
- emitter 仍固定 dirty-only。

### 文件和接口改动清单

#### 删除

- `compiler/mlir/include/pyc/Transforms/CombPartition.h`
- `compiler/mlir/lib/Transforms/PartitionCombPass.cpp`
- `compiler/mlir/lib/Transforms/CheckCombPartitionsPass.cpp`
- `compiler/mlir/test/supernode_partition_gate.sh`
- `compiler/mlir/test/check_supernode_partition.py`
- `compiler/mlir/test/check_comb_partition_contract.py`
- `compiler/mlir/test/Inputs/supernode_invalid_partition.mlir`
- `compiler/mlir/test/Inputs/supernode_optional_update.py`
- `compiler/mlir/test/Inputs/supernode_unused_livein.mlir`

#### 改名并保留

- `compiler/mlir/test/Inputs/supernode_nonmemoizable.mlir`
  → `compiler/mlir/test/Inputs/comb_nonmemoizable.mlir`
  - 保留 `arith.addi` 非 whitelist 负例；
  - 更新注释，不再提 supernode/partition。

#### 修改

- `compiler/mlir/include/pyc/Transforms/Passes.h`
  - 删除两个 factory。
- `compiler/mlir/CMakeLists.txt`
  - 删除两个 source。
- `compiler/mlir/tools/pyc-opt.cpp`
  - 删除两个 force-link。
- `compiler/mlir/tools/pycc.cpp`
  - 删除两个 option 和 parsing；
  - 删除输入/输出 partition verifier；
  - 删除 partition transform；
  - 恢复单一 `enableFuseComb` 条件；
  - 保留 `CheckCombMemoizablePass`。
- `compiler/mlir/test/change_schedule_gate.sh`
  - 增加两个 CLI hard-break 检查；
  - 增加 `comb_nonmemoizable.mlir` 负例。
- `CHANGELOG.md`
  - 记录删除 static partition hard break。
- `docs/change-driven-scheduler-implementation-20260923.md`
  - 删除 `fuse/partition` 和 partition-check 描述。
- 新增本文档并回写验证结果。

### 边界条件、错误处理与兼容性

- 旧带 `pyc.partition.*` 的 discardable attrs 不再有编译器合同；不增加兼容清理。
- 旧 CLI 直接失败。
- `pyc.comb.result_names` 只由 partition 使用，随合同删除。
- `arith.addi` 位于 comb body 的负例仍应被 memoization checker 拒绝。
- `CombOp::verify()` 的 memory-effect-free/nested-region 检查保留；它不仅服务 placement，
  也保护 memoization 和重排安全。

## 与既有文档和约束的一致性检查

- `docs/updatePLAN.md`：canonical graph、cycle、logic-depth gates 保留。
- Decisions 0127/0128/0134/0135：CombDepGraph 和 instance-aware legality 不变。
- dirty-only scheduler：memoizable comb、schedule planner/verifier 和 emitter 全保留。
- PCH 属于已接受的独立合入功能，本次不修改。

## 测试与验证计划

1. 静态零残留搜索。
2. 构建：

   ```bash
   cmake --build .pycircuit_out/toolchain/build --target pycc -j2
   cmake --build .pycircuit_out/toolchain/build \
     --target pyc4_primitive_change_reporting_test -j2
   ```

3. CLI hard break：
   - help 不含 `--comb-partition`/`--comb-partition-max-nodes`；
   - 传旧 option 失败。
4. memoization negative fixture 被拒绝，诊断包含：

   ```text
   is not on the deterministic pyc.comb memoization whitelist
   ```

5. 回归：

   ```bash
   tests/runtime/run_change_scheduler.sh
   compiler/mlir/test/change_schedule_gate.sh
   compiler/mlir/test/comb_dirty_scheduler_smoke.sh
   compiler/mlir/test/instance_vector_cache_smoke.sh
   .pycircuit_out/toolchain/build/runtime/cpp/pyc4_primitive_change_reporting_test
   ```

6. PCH/member placement 文件 diff 必须为空。
7. `git diff --check` 和文档 reader review。

已知 `change_schedule_gate.sh` 仍有 tampered fixture 诊断文本顺序问题；本任务不改变
verifier 诊断优先级，最终结果单独记录。

## 实施步骤

1. 删除 partition implementation/header 和 build/tool registration。
2. 简化 pycc CLI/pipeline，保留 memoization gate。
3. 删除 partition tests；迁移 memoization negative fixture。
4. 扩展 change schedule gate 的 hard-break/memoization 检查。
5. 更新 changelog 和现状文档。
6. 构建并运行验证计划。
7. 检视 PCH/member placement 零 diff。
8. 只提交本任务文件并回写结果。

## 待确认事项

无待确认架构项。请求批准：完全删除 `--comb-partition=static`、max-nodes option、
partition rewrite、partition verifier、`pyc.partition.*` metadata 和全部 supernode
专用测试；保留 `CombMemoization`/`CheckCombMemoizablePass`，并将唯一独立的
nonmemoizable 负例改名迁入 change schedule gate；PCH/member placement/compile
budget 不做任何修改。

## 实施与验证结果

实施日期：2026-09-23

### 已实施

- 删除 `CombPartition.h`、`PartitionCombPass.cpp` 和
  `CheckCombPartitionsPass.cpp`。
- 删除 factory、CMake source 和 pyc-opt force-link。
- 删除 pycc 的两个 CLI option、mode parsing、partition checks/transform 和
  conditional fuse 分支。
- 保留 pre/post `CheckCombMemoizablePass` 与 `FuseCombPass` shared whitelist。
- 删除 supernode/partition gate、Python checker 和三个专用 fixture。
- 将 `supernode_nonmemoizable.mlir` 改名为 `comb_nonmemoizable.mlir`，更新注释并
  迁入 `change_schedule_gate.sh`。
- 增加两个旧 CLI option 必须失败的 hard-break 检查。
- 更新 changelog 和 change-driven 实现说明。

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

- pycc help 不含两个 partition option；
- 传任一旧 option 均失败；
- `comb_nonmemoizable.mlir` 被 memoization whitelist gate 拒绝；
- local/cross-instance cycle 检查通过；
- canonical schedule 两次输出稳定。

PCH、member placement 和 frontend CLI 文件 diff 检查为空，未被本任务修改。

### 已知既有失败

`change_schedule_gate.sh` 最终仍因 tampered fixture 的诊断文本期望返回 1：

```text
期望：schedule fanout size mismatch
实际：change-driven schedule summary does not match canonical graph
```

tampered IR 已被正确拒绝；该问题与 partition 删除无关。
