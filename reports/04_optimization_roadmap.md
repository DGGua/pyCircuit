# gate-first 资源优化路线

## 1. 成功标准

优化不能只满足“Yosys 数字下降”。每项变更必须同时满足：

1. **G1 legality**：IR 合法、single-driver、no dynamic、flat types、无 comb loop、logic depth gate 通过；
2. **G2 equivalence**：C++ 与 Verilog/Verilator 在明确 observation point 下等价；
3. **结构收益**：在 pass 后 MLIR 或 Yosys cut point 能解释结构为什么减少；
4. **目标收益**：固定 technology flow 后 LUT/FF/BRAM/DSP 或 cell area 改善；
5. **可回归**：命令、版本、manifest、JSON 和阈值进入 CI。

## 2. P0：先修测量体系

### P0.1 建立三类 benchmark

- **micro**：delay sharing、one-hot mux、priority mux、sync/byte/multiport memory、reset/no-reset register；
- **block**：BypassUnit、RegisterFile、IssueQueue；
- **connected system slice**：真实实例连接的 frontend/backend 小切片，不使用并排 wrapper。

每个 case 同时提供 pyCircuit、手写 canonical SV；有可靠 Chisel 对照时再加入 Chisel。先用 Verilator/形式检查证明 latency/reset/RDW 一致，再比较资源。

### P0.2 新增三层 stats

1. `ir_definition_inventory`：保留现有语义，但明确是函数定义汇总；
2. `reachable_instance_stats`：从 top 展开，按 instance multiplicity 统计 reg/memory/mux/compare/算术，并保留 source loc/`pyc.name`；
3. `yosys_*_stats`：proc、memory、generic、target 四个 cut point 的 JSON。

建议给每类 cell 记录 `count + total input/output bits + reset/enable class`，仅记 op 个数会掩盖一个 1-bit mux 与一个 512-bit mux的差异。

### P0.3 固化复现实验

- pin Yosys/ABC 版本或容器 digest；
- 保存 top、parameters、defines、flatten、target、Liberty/family；
- CI 中 resource lane 不允许“找不到 yosys 就 skip”；
- 生成 `result.json` 时记录 git commit、dirty flag、脚本 hash 和输入 RTL hash；
- old artifact 与当前源码 hash 不匹配时拒绝进入趋势图。

验收：任意一名开发者可用一条命令复现同一 case 的各 cut-point JSON；重复运行结果稳定。

## 3. P1：补齐 correctness/optimization gates

### P1.1 single-driver / resolved-net verifier

对应 Decision 0130/0137。先让 MLIR gate 拒绝普通 wire 的 0 或多 driver，显式 resolved net 走单独 op；Emitter 不再在拓扑失败后继续打印。

验收用例：无 driver、单 driver、多 driver、显式 resolve、跨 instance comb loop；诊断包含层级路径和 source location。

### P1.2 reset intent 合同

对应 Decision 0115。为 state 明确至少两类语义：

- architectural/reset-required state；
- reset 后由 valid 屏蔽、初值不可观察的数据 pipeline state。

先在 dialect/verifier 定义，再让 C++ simulator 和 Verilog primitive 同时实现。禁止 Emitter 根据“看起来是 balance reg”自行删 reset。

验收：reset 前后 TICK-OBS/XFER-OBS litmus、unknown/uninitialized 可观察性检查、双 backend 等价。

### P1.3 one-hot 合同

为 mux/select op 增加已证明的 `onehot`/`onehot0` 属性或专用 op；静态可证明时由 pass 添加，用户声明时由 assertion/formal gate 保护。true priority 保持原语义。

验收：多 hit 反例必须区分 priority 和 one-hot；同一 one-hot case 至少比较 forward chain、masked OR 和 tree 的 target 资源。

### P1.4 memory 合同与 inference gate

对应 Decisions 0114/0122：明确 read latency、write commit、RDW old-data、byte strobe、reset/init、越界地址。增加 Yosys gate，确认期望 memory 在 `memory_collect` 后仍为 `$mem`，目标流程中映射成期望 BRAM/macro。

验收：如果 memory 意外展开为 FF，CI 直接失败并报告是哪个 op/source location。

## 4. P2：MLIR/frontend 结构优化

### P2.1 cycle-balance canonicalization

不要只在 Python 对象层加 cache。建议建立 MLIR pass：

1. 以 canonical SSA value、clock/reset domain、source cycle、target cycle、reset intent 为 key；
2. 为同一 value 构造共享 delay chain，较短消费者复用前缀；
3. vector delay 尽量保留为 vector-wide state，再按固定 flatten 规则 lowering；
4. CSE 后再次消除 duplicate stages；
5. 输出每条新增 pipeline 的 provenance 和 fanout。

在 pass 生效前先新增 gate/analysis；不要依赖当前已过时文档中不存在的 `CycleBalancePass`。

验收：micro case 精确减少预期 register bits；BypassUnit 要以当前源码重建数据，不复用旧 build 数字；latency 与 G2 必须不变。

### P2.2 register file / multiport memory lowering

优先研究目标相关的结构，而非仅压缩 RTL：

- bank + replicated read copies；
- live-value table/写旁路；
- FPGA BRAM 端口组合；
- ASIC SRAM/register-file macro；
- 小深度时保留 FF 实现，由 cost model 选择。

需要一个显式 multiport memory/regfile IR，而不是从任意 FF+mux 猜回 memory。冲突、RDW、端口 latency 必须是 op 合同。

验收：同功能模型下分别报告 abstract storage bits、复制因子、mux/decode、target macro/BRAM 数和 latency。

### P2.3 mux 与 vector canonicalization

- 将 `v_get(v_create(...), i)`、vector compare 后 extraction 等模式在 MLIR 中消去；
- proven-onehot 选择尝试 masked OR/tree；
- true priority 用分层 priority encoder + data mux，避免超长线性链；
- 扩展 SLP 到 add/sub/compare/shift 等已证明同构的 lanes；
- 所有 rewrite 先有等价 pattern test，再接 target resource test。

### P2.4 primitive inference quality

- 根据 target 生成清晰的 primitive/memory mapping policy；
- 避免 reset/enable pattern 阻止 SRL、BRAM、DSP 输入/输出寄存器吸收；
- 对综合器需要的 attribute 只作为 mapping hint，不能改变 dialect 语义；
- 保留 generic 与 target stats，防止某一 vendor 改善掩盖结构退化。

## 5. P3：Emitter 可读性与小优化

这一阶段排在结构优化之后：

- 在不影响 debug naming 合同的情况下少发射无用 alias/wire；
- 对 pack/unpack bridge 做稳定、易读的生成；
- topology sort 失败一律报错；
- 输出 op/source-location 到 Verilog signal 的 sidecar map；
- 对每个 module 输出 primitive summary。

这些工作能改善调试和下游工具输入规模，但必须用 post-Yosys 数据证明是否有 PPA 收益，不能用 Verilog 文件大小代替。

## 6. 建议的首批任务切分

| 顺序 | 任务 | 预期产物 | 关联决策 |
|---:|---|---|---|
| 1 | benchmark manifest + Yosys JSON runner | 可复现 micro/block baseline | 0112 |
| 2 | top-reachable instance stats | 正确的 IR 结构账本 | 0134/0135 |
| 3 | single-driver verifier | 修复当前 emitter 前的 legality 缺口 | 0130/0137 |
| 4 | memory inference litmus | RDW/reset/BRAM/macro gate | 0114/0115/0122 |
| 5 | balance provenance analysis | 每个 `_v5_bal` 的来源、bit、fanout | 0112/0115 |
| 6 | one-hot mux IR contract | 安全启用低成本 lowering | 0112/0130 |
| 7 | cycle-balance sharing pass | 减少重复/逐 lane pipeline | 0112/0115 |
| 8 | explicit multiport regfile op | 支持 bank/macro lowering | 0114/0122 |

## 7. 每个优化 PR 的报告模板

```text
Implements: <decision IDs>
Benchmark contract: <manifest path/hash>
Correctness gates: G1 ..., G2 ...
Toolchain: yosys <version>, target/liberty <id>

Before/after by cut point:
  raw IR:
  optimized IR:
  post-proc:
  post-memory:
  generic synth:
  target map:

Why it changed:
  <source op → pass rewrite → netlist cell>

Known trade-offs:
  latency/reset/memory replication/timing/debug naming
```

## 8. 第一阶段建议目标

第一阶段不要承诺一个未经基线证明的面积百分比。更可靠的目标是：

- 100% benchmark 有等价合同和可达 top；
- 100% 结果带工具/输入 hash；
- memory inference 失败可自动检测；
- balance register 可按来源解释到 bit；
- BypassUnit、RegisterFile 的差异能在某一个 cut point 首次定位；
- 所有结构优化保持 G1/G2 通过。

完成这些后，团队才具备持续把资源差距缩小、并防止回退的基础设施。

