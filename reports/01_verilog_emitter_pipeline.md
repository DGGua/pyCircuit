# 从 Emitter 到生成 Verilog

## 1. 主链路

```text
Python/C++ 模型写法
        │ frontend 展开、类型/周期推导
        ▼
     .pyc / MLIR
        │ pycc pass pipeline：合法化、优化、门禁
        ▼
  flat/static PYC IR
        │ VerilogEmitter（机械 lowering）
        ├──────────────► 每个 func.func 一个 .v
        ├──────────────► pyc_primitives.v
        ├──────────────► manifest.json
        └──────────────► yosys_synth.ys
```

语义边界在 MLIR，而不是 Emitter。Decision 0112 要求 C++ simulator 与 Verilog 逻辑等价；reset/init、memory RDW、net/var 等分别受 0115、0114/0122、0130 约束。参见 [decisions 0112 起](../docs/rfcs/pyc4.0-decisions.md#decision-0112-mlir-dialect-is-the-single-semantic-source-c-sim-and-verilog-emission-must-be-logically-equivalent)。

## 2. `pycc` 在 Emitter 前做了什么

当前 pipeline 位于 [pycc.cpp:2254](../compiler/mlir/tools/pycc.cpp#L2254)：

1. frontend contract、函数内联策略、可选 hierarchy flatten；
2. canonicalize、CSE、SCCP、SymbolDCE；
3. static SCF lowering；
4. wire 和 dead state 消除；
5. vector unroll 或 SLP packing；
6. comb canonicalize；
7. instance-aware comb cycle、clock domain、flat type、no-dynamic、logic-depth 门禁；
8. i1 register packing、comb fusion，再次 canonicalize/CSE；
9. 收集 MLIR compile stats；
10. 最后才进入 emitter。

这解释了一个重要调试原则：如果生成 RTL 中已经出现了 1000 个寄存器或 200 层 mux，先看 pass 后 MLIR，而不是先看 Verilog 字符串格式。

### 当前优化覆盖的边界

- [EliminateWiresPass](../compiler/mlir/lib/Transforms/EliminateWiresPass.cpp#L67) 只消除恰好一个 driver 的 wire；多 driver 会保留。
- `PackI1RegsPass` 是保守的相邻 i1-register packing，主要减少 primitive 实例数，不会减少状态 bit 数。
- `SLPPackWiresPass` 主要覆盖 AND/OR/XOR/EQ/NOT/MUX 的同构 scalar lanes；ADD/SUB/MUL/比较/移位等大批结构尚不在同一覆盖面。
- `CombCanonicalizePass` 有局部布尔、mux、vector pattern，后续仍依赖 Yosys 做通用逻辑化简。

## 3. Emitter 的 Verilog 组织

### 3.1 模块与端口

每个非 declaration 的 `func.func` 被发射为一个 Verilog module；`--out-dir` 模式遍历函数并写成独立 `.v` 文件，见 [pycc.cpp:2466](../compiler/mlir/tools/pycc.cpp#L2466)。

vector 端口在模块边界被压成一条 packed bus，内部仍使用 unpacked arrays。规则在 [VerilogEmitter.cpp:65](../compiler/mlir/lib/Emit/VerilogEmitter.cpp#L65)；pack/unpack 为逐 lane 连线，见 [VerilogEmitter.cpp:134](../compiler/mlir/lib/Emit/VerilogEmitter.cpp#L134)。

```verilog
module example (
  input  [127:0] in,
  output [127:0] out
);
wire [31:0] in__vec [0:3];
assign in__vec[0] = in[31:0];
// ...
```

这些切片 `assign` 通常在综合后只是连线，不应按 RTL 行数计面积。只有当桥接阻碍后续模式识别时，才可能产生间接成本，需要由综合网表 A/B 证明。

### 3.2 组合逻辑

scalar PYC ops 被发射为 continuous assignments，例如 add、mul、mux 见 [VerilogEmitter.cpp:321](../compiler/mlir/lib/Emit/VerilogEmitter.cpp#L321)。除零行为也已显式编码为比较加 mux，而不是交给 backend 猜测，见 [VerilogEmitter.cpp:346](../compiler/mlir/lib/Emit/VerilogEmitter.cpp#L346)：

```verilog
assign result = (rhs == 0 ? 0 : (lhs / rhs));
```

这类逻辑若语义要求如此就是必要成本；若要改变行为，必须在 dialect 层改约定并通过双 backend gate，不能只删掉 Verilog 中的保护逻辑。

vector elementwise op 当前由 Emitter 按 lane 展开。reduction 可按 op attribute 选择 chain 或 tree，入口见 [VerilogEmitter.cpp:579](../compiler/mlir/lib/Emit/VerilogEmitter.cpp#L579)。大规模 mux 的拓扑则主要在 frontend/MLIR 阶段形成。

所有 op result 都先声明为 wire，见 [VerilogEmitter.cpp:944](../compiler/mlir/lib/Emit/VerilogEmitter.cpp#L944)。这会增加 RTL 文本和信号名数量，但 alias/中间 wire 多数会被 `opt` 清除。

`pyc.assert` 包在 `` `ifndef SYNTHESIS `` 中，见 [VerilogEmitter.cpp:1056](../compiler/mlir/lib/Emit/VerilogEmitter.cpp#L1056)，因此不能解释综合面积增大。

### 3.3 子模块

`pyc.instance` 被解析为普通 Verilog module instance。vector 跨层级时会生成 `__flat` packed bridge，见 [VerilogEmitter.cpp:1089](../compiler/mlir/lib/Emit/VerilogEmitter.cpp#L1089)。

Emitter 会保持层级；是否 flatten 由 `pycc` hierarchy policy 和综合命令决定。资源比较必须固定同一 flatten 策略，否则 cell sharing、constant propagation 和模块复制结果都不可比。

### 3.4 时序 primitive

`pyc.reg/fifo/mem/async_fifo/cdc` 不直接展开在业务 module 中，而是实例化 `pyc_primitives.v`，入口见 [VerilogEmitter.cpp:1180](../compiler/mlir/lib/Emit/VerilogEmitter.cpp#L1180)。

最关键的 register 模板是 [pyc_reg.v](../runtime/verilog/pyc_reg.v)：

```verilog
always @(posedge clk) begin
  if (rst)
    q <= init;
  else if (en)
    q <= d;
end
```

因此每一个 `pyc.reg` 都有同步 reset、enable 和运行时 `init` 输入。对自动插入的宽数据流水寄存器，这可能：

- 增加 reset mux/control fanout；
- 阻碍 FPGA SRL/BRAM/DSP 周边寄存器吸收；
- 让 ASIC 标准单元只能选带 reset/enable 的更大 flop 或额外 mux。

是否允许“不复位的数据寄存器”属于 Decision 0115 的语义问题，必须为 C++/Verilog 同时建模并先加 verifier，不能在 Emitter 中偷偷去 reset。

同步 memory 使用 registered read、byte strobe 和 old-data RDW，见 [pyc_sync_mem.v:1](../runtime/verilog/pyc_sync_mem.v#L1)。`PYC_TARGET_FPGA` 下增加 block RAM 属性，见 [pyc_sync_mem.v:42](../runtime/verilog/pyc_sync_mem.v#L42)。memory array 本身只在仿真分支清零；reset 清的是 `rdata`，不是整块存储。

`pyc_byte_mem` 是组合读 byte array，见 [pyc_byte_mem.v:48](../runtime/verilog/pyc_byte_mem.v#L48)。组合读、多端口或复杂 byte-write pattern 是否能推成目标 BRAM/macro，必须在指定器件/techlib 流程中检查，不能只看 generic `synth`。

## 4. 已发现的语义门禁缺口

Emitter 的组合拓扑排序遇到同一 wire 多个 continuous driver 时返回失败，见 [VerilogEmitter.cpp:819](../compiler/mlir/lib/Emit/VerilogEmitter.cpp#L819)；但调用者失败后只是改用字典序继续发射，见 [VerilogEmitter.cpp:1048](../compiler/mlir/lib/Emit/VerilogEmitter.cpp#L1048)。同时，`AssignOp::verify()` 只验证 `dst` 由 `pyc.wire` 定义，见 [PYCOps.cpp:909](../compiler/mlir/lib/Dialect/PYC/PYCOps.cpp#L909)。

这与 Decision 0130 的 single-driver var / explicit resolved-net 合同之间存在缺口。正确处理顺序是：

1. 先增加 MLIR-level driver legality verifier，并给出层级/源位置诊断；
2. 为确需多 driver 的情形引入显式 resolved-net 语义；
3. C++ 与 Verilog backend 同时消费已验证 IR；
4. Emitter 对不合法 IR 直接失败，而不是排序后继续。

它首先是 correctness 问题，也会影响优化器能否安全消除 wire 和共享表达式。

## 5. 文档与实现漂移

[cycle_balance_improvement.md](../docs/cycle_balance_improvement.md#方案总览) 描述了 `CycleBalancePass.cpp`、`dst_cycle/src_cycle` 以及共享 delay cache；当前非文档源码中没有这些符号，`pycc` pipeline 也没有 `createCycleBalancePass`。实际 V5 cycle-aware 路径是在 frontend 的 [`delay_to`](../compiler/frontend/pycircuit/v5.py#L308) 循环创建 `_v5_bal_N` register。

做优化前应把这份文档标为历史设计或更新为当前实现，避免按不存在的 pass 定位问题。项目中同时出现 pyc4.0/0.40、V5、V6 名称；本次分析以 `AGENTS.md` 指定的 pyc4.0 decisions 为规范来源，以当前源码为事实来源。

## 6. 推荐的阅读/定位顺序

针对一个面积异常模块：

1. 在 frontend 输出的 `.pyc` 中统计 `pyc.reg`、memory、mux、compare，并按 `pyc.name`/location 找来源；
2. 保存 pass 后 MLIR，确认哪些结构被 canonicalize/CSE/dead-state 删除；
3. 查看生成 `.v`，确认 primitive 类型、层级、宽度和 reset/enable；
4. 在 Yosys 的 `proc`、memory collect、generic synth、technology map 四个 cut point 分别导出 JSON stats/netlist；
5. 只有差异第一次出现在哪一层被定位后，才决定改 frontend、MLIR pass、primitive pattern 还是综合约束。

