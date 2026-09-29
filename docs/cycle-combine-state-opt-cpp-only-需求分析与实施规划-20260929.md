# 状态优化仅 C++ 路径启用需求分析与实施规划

## 背景与目标

`pycc` 删除策略开关后，C++ 与 Verilog 都固定跑 `pyc-strip-state-observability`、`pyc-combine-delay-chains`、`pyc-retime-pipelines`、`pyc-pack-state-lanes`。用户要求这几个本分支新加的 pass 只在生成 C++ 时触发，避免改写 Verilog 综合网表。

## 项目现状与执行流程定位

`compiler/mlir/tools/pycc.cpp` 在解析 `--emit` / `-cpp` / `-verilog` 之后组装 PassManager。状态优化插在 eliminate-wires / dead-state 之后、SLP 与 legality gate 之前。`--emit=none` 仍跑完整 pass 管线，只是不落盘。

## 相关现有实现说明

新加改写 pass：strip（及随后的 dead-state）、三轮 combine（中间夹 canonicalize/CSE）、retime（及随后的 dead-state）、pack-state-lanes。`pyc-pack-i1-regs` 是旧 pass，不在本次门控范围。`pyc-opt` 仍可单独指定这些 pass。

## 需求与验收标准

1. `--emit=cpp` 或 `-cpp`：行为与现在相同，structural + pipeline。
2. `--emit=verilog` / `-verilog` / `--emit=none`：不跑上述新加 pass，也不跑仅为它们服务的额外 canonicalize/CSE / dead-state。
3. 统计：C++ 为 `structural`/`pipeline`；其它 emit 为 `off`/`off`，`state_opt_pack_width=0`。
4. 优化后的 C++ 与未优化 Verilog 在对照夹具上功能结果仍一致。

## 方案设计

### 模块边界、输入与输出

只改 `pycc` 管线装配与统计字段。Pass 实现、`pyc-opt` 不变。

### 上下游关系与数据/控制流

`emitKind` 在命令行解析后已确定。用 `emitKind == "cpp"` 包住状态优化整段。Verilog 保持前端寄存器网表。

### 文件和接口改动清单

- `compiler/mlir/tools/pycc.cpp`：按 emit 门控，写回政策名。
- `state_delay_optimization_smoke.sh`：Verilog 断言改为 off。
- `check_state_retime_models.py` / `check_state_delay_tap_models.py`：C++ 验改写，Verilog 验未改写，仿真结果仍对齐。
- `docs/delay_line.md`、`docs/MLIR_PASS_PIPELINE.md`：条件改为仅 `--emit=cpp`。

### 边界条件、错误处理与兼容性

`--emit=none` 不做状态优化。外部若指望 Verilog 默认 delay_line/retiming，需改走 `pyc-opt` 或先 `--emit=cpp`。

## 与既有文档和约束的一致性检查

覆盖此前「C++/Verilog 都做」的文档结论。gate-first 仍在 MLIR；Verilog 只是不再走这组改写。

## 测试与验证计划

增量编 `pycc`，跑 `state_delay_optimization_smoke.sh`、`delay_line_diagnostics_smoke.sh`。

## 实施步骤

1. 改 `pycc.cpp`。
2. 改测试与文档。
3. 编译并跑 smoke。

## 待确认事项

无。已拍板：新加状态优化 pass 仅 `--emit=cpp` 运行。

## 实施结果

已改完，尚未提交。

- `--emit=cpp`：`state_opt_policy=structural`，`state_retime_policy=pipeline`。
- `--emit=verilog` / `--emit=none`：政策为 `off`，不跑 strip/combine/retime/pack-state-lanes。
- `delay_line_diagnostics_smoke.sh`、`state_delay_optimization_smoke.sh`：PASS。优化后的 C++ 与未优化 Verilog 对照结果一致。
