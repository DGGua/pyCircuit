# C++ 与 Verilog trace-config 对齐需求分析与实施规划

## 背景与目标

PyCircuit 的 `--trace-config` 已经生成一份共享 `TracePlan`，C++ testbench 和
SystemVerilog testbench 都消费该计划。端口在两份 VCD 中可以一一对应，但当前
内部 `.named()` alias 只会进入 Verilog/Verilator VCD，不会进入 C++ VCD。

已用最小设计验证：

- trace plan 同时选择了 `dut:debug_sum_alias`；
- SV testbench 生成
  `$dumpvars(0, ..., dut.debug_sum_alias, ...)`；
- Verilator VCD 包含
  `tb_alias_name_check.dut.debug_sum_alias`；
- C++ testbench 的 enabled signal 表包含 `dut:debug_sum_alias`，但
  `pyc_trace_vcd()` 没有注册它，因此 C++ VCD 不包含该信号。

本计划的目标是让两个后端严格消费同一份 `TracePlan`：

1. 相同端口和内部命名值被选择；
2. 未被选择的内部值不因 trace-config 增加 C++ 持久存储；
3. 两份 VCD 经 canonical path 规范化后具有相同的目标信号集合；
4. trace window 和无 trace-config 行为保持兼容。

本计划取代
`vcd-alias-cross-backend-requirements-and-plan-20260825.md` 中“直接固定修改
DavinciTop”的默认范围。先修复并验证框架，不修改 DavinciTop 设计。

## 项目现状与执行流程定位

当前 build 流程如下：

1. Python frontend 生成每个模块的 `.pyc` 和项目 manifest。
2. `pycc --emit=none --probe-manifest` 生成 probe catalog。
3. frontend 合并端口、`.named()` 值和 `@probe`，生成 `probe_manifest.json`。
4. `compute_trace_plan_from_artifacts()` 根据 `--trace-config` 生成
   `trace_plan.json`，其中：
   - `enabled_signals` 使用 `<instance_path>:<field_path>`；
   - `enabled_instances` 决定递归进入哪些实例；
   - `signal_obs` 和 `window` 描述采样点与窗口。
5. frontend 分别生成 C++ 和 SV testbench：
   - C++ 把 `enabled_signals` 交给生成模块的 `pyc_trace_vcd()`；
   - SV 把同一 canonical path 转成层级引用并生成 `$dumpvars`。
6. device C++ 由 `CppPlacementPass` 决定 comb value 是方法局部变量还是类成员。

当前缺口发生在步骤 6 和 C++ emitter：

- 位于 `pyc.comb` 内且未跨边界的 alias 被 placement 设为 Local；
- Local alias 在 `eval_comb_*()` 返回后没有可供 VCD writer 持续读取的地址；
- `pyc_trace_vcd()` 只生成端口注册代码；
- trace plan 虽然选中了 alias，C++ device code 却没有可注册对象。

## 相关现有实现说明

### 统一选择语义

`compiler/frontend/pycircuit/trace_dsl.py` 已经是唯一选择语义来源。不能在 C++
emitter 中重新实现 glob、family、tag、stage、lane 匹配，否则会再次产生后端
漂移。

### Verilog 路径

`_render_tb_sv()` 将 canonical path：

```text
dut:debug_sum_alias
```

转换为：

```text
dut.debug_sum_alias
```

并交给 `$dumpvars`。Verilog emitter 已为 `pyc.alias` 生成稳定同名 net。

### C++ 路径

`_render_tb_cpp()` 已正确生成相同的 enabled signal 集合。缺失部分是：

1. selected internal value 没有被 placement 固定为类成员；
2. `CppEmitter.cpp` 生成的 `pyc_trace_vcd()` 没有遍历 named internal value。

## 需求与验收标准

1. `TracePlan.enabled_signals` 是 C++ 与 Verilog 的唯一信号选择结果。
2. C++ 只为被 trace plan 选中的 Local named value 增加持久类成员。
3. 已经是 Struct 的 selected named value 不重复增加存储。
4. `pyc_trace_vcd()` 同时支持端口和 selected named internal value。
5. 没有 `--trace-config` 时：
   - C++ placement 结果与当前版本一致；
   - 不因 `.named()` 普遍增加类成员；
   - 现有 `PYC_TRACE_DIR` 顶层端口 VCD 行为保持不变。
6. 相同配置下，C++ 和 Verilator VCD 的目标信号集合经以下规范化后相同：
   - C++：`dut:field`；
   - Verilog：`dut.field`；
   - 比较身份统一为 canonical `dut:field`。
7. 同名端口和 alias 的位宽和值序列一致。
8. 修改 trace-config 后，C++ device cache 必须失效；未改变配置时继续命中。
9. trace window 的起止周期在两个后端保持现有一致语义。

## 方案设计

### 模块边界、输入与输出

新增一份仅供 codegen 使用的派生计划，建议命名为
`trace_codegen_plan.json`：

```json
{
  "version": 1,
  "modules": {
    "alias_name_check": ["debug_sum_alias"]
  }
}
```

输入来自已经解析完成的：

- `trace_plan.enabled_signals`
- `probe_manifest` 中 canonical path 到 module/field 的映射

输出按模块列出需要在 C++ 中具有稳定 storage 的内部 field。端口无需列入，
因为它们天然是成员。

### 上下游关系与数据/控制流

```text
--trace-config
      |
      v
compute_trace_plan_from_artifacts
      |
      +----> SV TB: canonical path -> $dumpvars
      |
      +----> trace_codegen_plan.json
                     |
                     v
              pycc C++ pipeline
                     |
                     v
       selected alias pinned to Struct
                     |
                     v
        pyc_trace_vcd registers alias
```

选择规则仍只在 frontend 执行一次；C++ codegen plan 只是已经解析结果的投影，
不重新解释 glob 或 tags。

### C++ storage 方案

不采用“所有 `pyc.name` 都固定为 Struct”的简单方案。DavinciTop 等大型设计由
JIT 产生大量命名中间值，全部持久化会破坏 `CppPlacementPass` 的局部变量优化，
增加类体积、编译成本和运行时内存访问。

采用按 trace-config 精确 pin：

1. `pycc` 新增内部参数，例如
   `--trace-codegen-plan <json>`。
2. pycc 在运行 C++ placement 前，将当前 module 中匹配 field 的 value 标记为
   transient trace-selected metadata。
3. `CppPlacementPass::pinToStruct()` 将 selected value 判定为必须 Struct。
4. placement summary 的 `probe_pinned_struct` 正确计数这些值。
5. 未选择值继续按现有 Local/Struct 规则放置。

该 metadata 只影响 C++ storage，不改变 `pyc.alias` 的硬件语义。

### C++ VCD 注册方案

重构 `CppEmitter.cpp` 的 named-value 收集，使 ProbeRegistry 和
`pyc_trace_vcd()` 复用同一份：

- field path；
- C++ storage 名；
- type/width；
- reg/comb kind。

`pyc_trace_vcd()` 为所有具有稳定 storage 的 named value生成
`trace_port(value, field_path)`。运行时 `enabledSig(canonical_path)` 再决定是否
真正调用 `tb.vcdTrace()`。

这样：

- selected Local alias 已被 placement 提升，可注册；
- 原本就是 Struct 的命名值无需特殊处理；
- 未选信号即使存在成员，也不会进入 VCD。

### 文件和接口改动清单

计划修改：

- `compiler/frontend/pycircuit/cli.py`
  - 从 trace plan 和 probe manifest 派生 module-field codegen plan；
  - 保存 JSON；
  - 仅在 C++ device pycc 命令中传入；
  - 将每个 module 的 selected field 集合纳入增量缓存键。
- `compiler/mlir/tools/pycc.cpp`
  - 新增 trace codegen plan 参数；
  - 校验 schema/version/module/field；
  - 在 C++ placement 前标记 selected values。
- `compiler/mlir/lib/Transforms/CppPlacementPass.cpp`
  - selected named value 固定为 Struct；
  - 修正 `probe_pinned_struct` 统计。
- `compiler/mlir/lib/Emit/CppEmitter.cpp`
  - `pyc_trace_vcd()` 注册持久 named internal value；
  - ProbeRegistry 与 VCD 共用 named-value catalog。
- 相关测试文件
  - frontend trace-plan/codegen-plan 单元测试；
  - C++ placement/emitter 测试；
  - C++/Verilator VCD 集成测试。

不计划修改：

- `pyc.alias` dialect 语义；
- Verilog emitter 的 alias net 生成；
- DavinciTop；
- canonical path 格式；
- trace-config DSL 格式。

### 边界条件、错误处理与兼容性

- codegen plan 中不存在的 module 或 field 必须报清晰错误，不能静默忽略。
- 同一 module 多实例中任一实例选择某 field，该 module class 都需持久化该
  field；VCD 注册仍由每个实例的 canonical path 独立过滤。
- 同名 field 的生成语言 sanitization 不作为身份；身份始终是 canonical path。
- Verilator 可能让 alias 与源信号共享 VCD id，这是合法别名，不应视为缺失。
- selected vector/aggregate 继续沿用现有 `vcdTrace(Vec)` 展开规则。
- sidecar C++ TB 当前明确不支持 trace-config，此限制保持并继续报错。
- trace-config 为空或未传入时，不生成 codegen plan 参数。

## 与既有文档和约束的一致性检查

已检查：

- `AGENTS.md`
- `docs/updatePLAN.md`
- `docs/rfcs/pyc4.0-decisions.md`
- `docs/v6_PyCircuit_Specification.md`
- `CONTRIBUTING.md`

一致性结论：

- Decision 0145 要求同一 trace config 同时供 C++ 和 Verilog tracing 使用；
  本方案直接补齐该要求。
- Decision 0147 要求 debug/probe 名来自显式用户名称，并保持增量构建稳定；
  module-field plan 和缓存键满足该要求。
- 不在 C++ emitter 重做 DSL 匹配，避免 backend-only 选择语义。
- 仅 selected value 被 pin，避免破坏大型设计的 C++ placement 优化。

当前 checkout 未发现 `AGENTS.md` 所引用的 `$pyc4` 和 `$pyc-build-v40`
skill 文件；实施时继续以 decision 文档和 gate 文档为权威。

## 测试与验证计划

### 1. 选择语义测试

最小层级设计包含：

- 输入/输出端口；
- `debug_sum_alias`；
- 一个未选择的 `unused_debug_alias`；
- 两个相同 module 实例。

验证 codegen plan：

- 只包含 trace-config 选中的 module-field；
- 多实例选择正确归并到 module；
- 无配置时为空。

### 2. Placement 测试

检查 MLIR placement metadata：

- selected alias 为 Struct；
- 未选择 alias 保持 Local；
- `probe_pinned_struct` 增加准确；
- 修改 trace selection 不改变硬件 IR 和 Verilog。

### 3. 生成文本测试

检查：

- C++ class 中存在 selected alias 成员；
- `pyc_trace_vcd()` 包含 alias 的 `trace_port`；
- enabled signal filter 仍使用 canonical path；
- SV `$dumpvars` 与现有结果一致。

### 4. 真实双后端 VCD 测试

用相同 testbench 生成两份 VCD，并通过
`systemverilog-waveform-debug-skill` 读取：

- C++：`tb_alias_name_check.dut:debug_sum_alias`
- Verilator：`tb_alias_name_check.dut.debug_sum_alias`

规范化为 `dut:debug_sum_alias` 后：

- selected signal 集合相同；
- 位宽相同；
- 时钟边沿值序列相同；
- 未选择 alias 不出现。

### 5. Window 测试

配置 begin/end cycle，验证：

- C++ `setVcdWindow`；
- SV `$dumpon/$dumpoff`；
- 两份 VCD 的有效采样窗口一致。

### 6. 增量构建与性能测试

- 相同 trace-config 再构建必须 cache hit。
- 改变某 module 的 alias selection，只重建受影响 C++ module 和 C++ TB。
- 无 trace-config 的 DavinciTop 生成规模、placement summary 和运行性能不变。
- 有小规模 selection 时，仅新增相应 struct members。

### 7. Gates

按可用环境运行：

- 目标 Python tests；
- pycc build；
- G0；
- 最小 C++/Verilator cross-backend test；
- 必要时 G1/G2 相关子集。

## 实施步骤

1. 将当前最小复现固化为失败测试：Verilator 有 alias，C++ 缺失。
2. 在 frontend 生成 per-module trace codegen plan，并加入缓存键测试。
3. 为 pycc 增加 plan 读取、schema 校验和 selected-value 标记。
4. 修改 CppPlacementPass，只 pin selected named values。
5. 重构 CppEmitter named-value catalog，并补齐 `pyc_trace_vcd()` 注册。
6. 运行最小双后端 VCD 测试，使用波形工具比较规范化信号和值。
7. 验证 trace window、无配置兼容性和增量构建。
8. 运行相关 gates，并将实际结果回写本规划文档。

## 待确认事项

无必须由用户选择的技术分支。推荐按“精确 pin selected alias”实施，不采用
“所有 named value 永久成员化”的低成本但高性能风险方案。

## 实施与验证结果（2026-08-26）

已按本规划完成：

- frontend 从 `TracePlan.enabled_signals` 和 `probe_manifest.json` 派生
  `trace_codegen_plan.json`；
- C++ module cache key 包含每个 module 的 selected internal field 集合，
  Verilog device cache key 保持与 trace selection 无关；
- `pycc --trace-codegen-plan` 严格校验 schema、字段类型和当前 module 中的
  field 是否存在；
- `CppPlacementPass` 只将原本为 Local 的 selected alias 提升为 Struct，
  `probe_pinned_struct` 只统计实际提升；
- `pyc_trace_vcd()` 与 ProbeRegistry 共用稳定 named-value catalog，并继续由
  canonical `enabledSig` 做最终过滤；
- 无 `--trace-config` 时不传 codegen plan，alias 保持 Local，C++ VCD 仍只含
  原有顶层端口。

新增测试：

- `tests/test_trace_codegen_plan.py`
  - 只选择 enabled internal field；
  - 多实例选择按 module 去重、排序；
  - 无 trace plan 时为空。
- `tests/test_trace_config_vcd_alias.py`
  - selected alias 的 plan、placement、生成文本和非法 field 报错；
  - C++ `dut:debug_sum_alias` 与 Verilator `dut.debug_sum_alias` 规范化对应；
  - 两边位宽均为 8，变化序列均为
    `0x00@0ns -> 0x08@4ns -> 0x11@6ns`；
  - `begin_cycle=0,end_cycle=1` 的有效 alias 采样一致；
  - 相同配置为 0 个 pycc job；改变 alias selection 后只重建 C++ device
    module 和两份依赖 trace plan 的 TB，Verilog device module 保持不变；
  - ports-only 与无配置构建不产生 alias 成员，placement 统计恢复基线。

已通过：

- `cmake --build .pycircuit_out/toolchain/build --target pycc -j2`
- `pytest tests/test_trace_codegen_plan.py -q`：3 passed
- `pytest tests/test_trace_config_vcd_alias.py -q`：1 passed
- `python3 flows/tools/check_api_hygiene.py ...`
- `bash flows/scripts/run_semantic_regressions_v40.sh`
- `git diff --check`
- 新增测试文件的 `ruff check`

环境中没有 Python `black` module；仓库现有 `cli.py` 全文件 `ruff check`
仍有与本改动无关的历史告警，因此没有对该大文件执行自动全量修复。

窗口补充说明：当前生成的 C++ `setVcdWindow` 和 SV `$dumpon/$dumpoff` 控制
保持原实现不变，本次回归验证配置窗口内的目标 alias 样本一致。单独实验确认
当前 Verilator 5.050 不会用 `$dumpoff` 截断原始 VCD 后续记录；若验收要求
物理 VCD 文件在 end cycle 后完全无记录，需要另行实现 Verilator harness
级 gating 或 VCD 后处理，不属于本次 selected-alias storage/registration 修复。
