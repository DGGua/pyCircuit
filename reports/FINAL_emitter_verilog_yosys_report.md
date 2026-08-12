# pyCircuit 最终主报告：Verilog 生成、RTL 验证、Yosys 综合与资源问题

## 1. 一页结论

pyCircuit 当前的核心链路是：

```text
Python 硬件 DSL
      │ elaboration / JIT
      ▼
PYC dialect on MLIR
      │ legality gates + structural passes
      ▼
优化后的静态 PYC IR
      │
      ├── VerilogEmitter ──► Verilog RTL ──► Verilator：功能仿真
      │                                  └─► Yosys：综合与资源分析
      │
      └── CppEmitter ──────► native C++ simulator
```

 先在 MLIR 中确认“生成了什么硬件”，再用 Verilog 检查“它被怎样表达”，最后用 Yosys 判断“它被映射成什么资源”。

## 2. Emitter 的输入已经不是原始 Python

虽然本文从 Emitter 开始，但必须先明确它接收到什么。

[`pycc.cpp`](../compiler/mlir/tools/pycc.cpp#L2254) 会在调用 Emitter 前完成：

```text
frontend contract / hierarchy 检查
        ↓
canonicalize、CSE、SCCP、dead code 清理
        ↓
静态展开 scf.for，scf.if lower 为 mux
        ↓
wire、dead state、dead instance 消除
        ↓
vector unroll 或 SLP packing
        ↓
组合环、CDC、flat type、no-dynamic、logic-depth 门禁
        ↓
i1 register packing、comb fusion、最终清理
        ↓
VerilogEmitter / CppEmitter
```

所以 Emitter 看到的不是：

```python
for i in range(8): ...
if valid: ...
```

而是已经确定数量和连接关系的：

```mlir
pyc.add
pyc.mux
pyc.reg
pyc.sync_mem
pyc.instance
```

## 3. Counter：从一段 DSL 看见真实硬件

仓库中的 Counter 位于 [`designs/examples/counter/counter.py`](../designs/examples/counter/counter.py#L13)：

```python
def build(m, domain, width=8):
    enable = cas(domain, m.input("enable", width=1), cycle=0)
    count = domain.signal(width=width, reset_value=0, name="count")

    m.output("count", wire_of(count))

    domain.next()
    count.assign(count + 1, when=enable)
```

它表达的是：

```text
enable = 1：count 更新为 count + 1
enable = 0：count 保持
reset：count 回到 0
```

对应的经典 RTL 模型是：

```text
                      ┌──────────┐
count ──► +1 ───────►│ enable   │──► next_count
  │                   │ mux      │
  └──────────────────►│          │
                      └──────────┘
                            │
                            ▼
                    [8-bit register]
                            │
                            └──► count
```

## 4. Verilog Emitter 的生成步骤

Verilog 后端集中在 [`VerilogEmitter.cpp`](../compiler/mlir/lib/Emit/VerilogEmitter.cpp)。完整 module 的入口是 [`emitFunc`](../compiler/mlir/lib/Emit/VerilogEmitter.cpp#L888)。它可以理解为六步。

### 第一步：一个 `func.func` 变成一个 Verilog module

PYC 用 `func.func` 表示一个硬件模块定义，`arg_names/result_names` 提供端口名称。Emitter 输出：

```verilog
module <symbol> (
  input  ...,
  output ...
);
```

每个 module 定义只输出一次；`pyc.instance` 才表示设计中真正实例化一份子模块资源。

vector 端口在边界上压成 packed bus，内部需要时再拆成 lane array。由此产生的切片 assign 通常只是连线，不应按行数估算面积。

### 第二步：为 SSA result 声明 Verilog signal

MLIR 中每条 op 的 result 都需要一个可引用名字。Emitter 的 `NameTable`：

- 优先使用 `pyc.name`；
- 否则根据 op 类型和编号生成名字；
- 做 identifier 清洗和去重。

因此 Counter 中会看到：

```verilog
wire [7:0] pyc_add_11;
wire [7:0] pyc_mux_12;
wire [7:0] pyc_reg_7;
```

这些名字有助于从 Verilog 追回 MLIR，但 wire 数量本身不是资源数量。Yosys 的 `opt` 通常会删除纯 alias 和无意义的中间连接。

### 第三步：把组合 op 发射成 continuous assignment

常见映射十分直接：

```mlir
%sum = pyc.add %a, %b : i8
%y = pyc.mux %sel, %sum, %q : i8
```

```verilog
assign sum = a + b;
assign y = sel ? sum : q;
```

主要组合 op 包括：

| PYC op | Verilog 结构 |
|---|---|
| `add/sub/mul` | 算术表达式 |
| `and/or/xor/not` | 位逻辑 |
| `eq/ult/slt` | 比较器 |
| `mux` | `sel ? a : b` |
| `extract/concat/cast` | slice、拼接和扩展 |
| vector element-wise | 按 lane 生成表达式 |
| vector reduce | chain 或 balanced tree |

Emitter 会对组合 op 建依赖图并进行拓扑排序，见 [`topoSortCombOps`](../compiler/mlir/lib/Emit/VerilogEmitter.cpp#L788)。这样可以让 Verilog 更接近信号依赖顺序，也方便阅读和后续工具处理。

当前有一个应在 MLIR 层补强的缺口：同一 `pyc.wire` 多 driver 会使拓扑排序失败，但调用侧会退回 lexical order，并没有统一 legality diagnostic。按照 Decision 0130，single-driver/resolved-net 应先由 dialect/pass 定义并检查。

### 第四步：把 state op 变成 primitive instance

Emitter 不为每个 register 临时拼一段 `always`，而是实例化统一的 [`pyc_reg.v`](../runtime/verilog/pyc_reg.v)：

```verilog
always @(posedge clk) begin
  if (rst)
    q <= init;
  else if (en)
    q <= d;
end
```

类似地：

| PYC state op | Verilog primitive |
|---|---|
| `pyc.reg` | `pyc_reg` |
| `pyc.fifo` | `pyc_fifo` |
| `pyc.byte_mem` | `pyc_byte_mem` |
| `pyc.sync_mem` | `pyc_sync_mem` |
| `pyc.sync_mem_dp` | `pyc_sync_mem_dp` |
| `pyc.async_fifo` | `pyc_async_fifo` |
| `pyc.cdc_sync` | `pyc_cdc_sync` |

所以排查 state 行为时，要同时看四层：

```text
PYC op/verifier
    → Emitter 怎样接参数和端口
    → runtime/verilog primitive
    → runtime/cpp 对应 primitive
```

只改 `pyc_reg.v` 的 reset 行为会让 C++ 与 Verilog 漂移；只在 Emitter 中删 reset 也违反“MLIR 是语义唯一来源”的合同。

### 第五步：保持 module hierarchy

`pyc.instance` 变成普通 Verilog instance：

```verilog
Child u_child (
  .a(...),
  .b(...),
  .y(...)
);
```

是否 flatten 不由 Emitter 单方面决定，而由 `pycc` 策略和综合流程决定。比较 pyCircuit、Chisel 和手写 RTL 时必须固定 flatten 策略，否则跨模块常量传播、共享和 dead-instance 删除程度不同。

### 第六步：写出工程产物

`pycc --emit=verilog --out-dir ...` 通常产生：

```text
<module>.v             每个硬件 module
pyc_primitives.v       runtime primitives 的拼接文件
manifest.json          top 与 Verilog 文件列表
compile_stats.json     MLIR definition inventory
yosys_synth.ys         Yosys sanity synthesis 脚本
```

Counter 的 [`manifest.json`](../designs/examples/counter/build/verilog/manifest.json) 指定 top 为 `counter`，并列出 `pyc_primitives.v` 与 `counter.v`。

## 5. 生成 Verilog 后怎样验证

生成结果至少有两条独立验证路径。

### 5.1 Verilator：验证 RTL 行为

Counter TB 位于 [`tb_counter.py`](../designs/examples/counter/tb_counter.py#L16)：

```python
tb.clock("clk")
tb.reset("rst", cycles_asserted=2, cycles_deasserted=1)
tb.drive("enable", 1)
tb.expect("count", 1)
tb.next()
tb.expect("count", 2)
```

`pycircuit build --target both --run-verilator` 的 Verilog 路径大致是：

```text
Tb actions
   │ cli.py::_render_tb_sv
   ▼
SystemVerilog testbench + DUT + pyc_primitives.v
   │ verilator --binary --timing --trace
   ▼
可执行 RTL simulator
   │
   ├─ drive/expect
   ├─ assertion
   └─ VCD trace
```

Verilator回答的是：

- reset 后值是否正确；
- enable、流水 latency 和 memory RDW 是否符合预期；
- C++ simulator 与 RTL 是否在相同观察点一致。

它不回答 LUT、FF、BRAM 或 ASIC 面积。


## 6. Yosys：把 RTL 变成电路结构

Counter 自带的 [`yosys_synth.ys`](../designs/examples/counter/build/verilog/yosys_synth.ys) 是：

```tcl
read_verilog -sv pyc_primitives.v
read_verilog -sv counter.v
hierarchy -top counter
proc; opt; memory; opt
synth -top counter
```

各阶段可以这样理解：

```text
read_verilog
  读取 RTL 语法并建立内部表示

hierarchy -top counter
  解析实例关系，删除 top 不可达 module

proc
  把 always/case/if 等过程语句 lower 为 mux、DFF、逻辑网络

opt
  常量传播、dead logic、alias 和冗余逻辑清理

memory
  识别、收集或映射 memory 结构

synth
  生成 generic netlist，并执行更完整的优化/映射
```

Yosys 回答的典型问题是：

- 有多少 DFF、mux、比较器、加法器；
- memory 是否仍然是 `$mem`，还是已经展开；
- generic 逻辑规模如何；
- 使用目标 FPGA/Liberty 流程后映射成多少 LUT/FF/BRAM/DSP 或标准单元面积。


## 7. 资源为什么可能高于 Chisel/手写 RTL（Cycle Aware的额外状态记录似乎在LinxISA/PyCircuit中修复）

资源差通常不是“Emitter 打印得不漂亮”，而是以下因素的乘积：

```text
算法/微结构
  × 自动流水和 reset 策略
  × mux/比较网络拓扑
  × memory 端口与推断模式
  × hierarchy/constant propagation
  × 目标技术映射
```

### 7.1 自动周期对齐：Counter 已经展示了机制

Counter 生成快照中的 `_v5_bal_1` 说明：cycle-aware alignment 会增加真实 state。

在大设计中成本近似为：

```text
额外 FF bits ≈ 信号宽度 × 对齐拍数 × 未共享的分支数
```

如果每一级都带 reset，还会增加 reset fanout，影响 FPGA SRL 推断、寄存器吸收和 ASIC flop 选择。

优化方向不是在 Emitter 中删除某个名字，而是：

- 在 MLIR 中统计每条 delay edge 的来源、宽度和拍数；
- 合并相同 value/domain/target-cycle 的 delay；
- 对 vector 做整体 delay，而不是 lane 级重复；
- 区分 architectural state 与 reset 后尚未 valid 的 data pipeline；
- reset intent 的变化必须由 dialect 和双 backend gate 支持。

### 7.2 Register File：FF 阵列加大 mux

[`pycircuit.lib.regfile`](../compiler/frontend/pycircuit/lib/regfile.py) 的结构基线把 entry 写成一组 registers，并为每个读口遍历 entry，形成：

```text
每个 entry：地址比较器
所有 entry：priority/select mux
每个写口：entry write decode
```

如果 Chisel/手写版本使用 SRAM macro、banking、复制读 bank 或专用多端口 regfile，而 pyCircuit 使用可复位 FF 加全表 mux，两者资源必然不在同一量级。这首先是微结构和 memory mapping 差异，不是语言高低。

正确方向是在 PYC 层表达明确的 regfile/multiport-memory 语义，再实现 banking/replication/macro mapping，而不是让 Verilog Emitter 把任意 register 数组“猜成 memory”。

### 7.3 Priority mux 与 one-hot 信息

[`Vec.priority_mux`](../compiler/frontend/pycircuit/hw.py#L3427) 默认必须保持真正的 priority 语义。如果 selector 实际 one-hot，但 IR 没有携带这一保证，Emitter/Yosys 只能保守实现 priority chain。

大规模选择网络的可能差异是：

```text
priority chain：语义安全，但深度和映射可能较差
masked OR/tree：适合已证明 one-hot 的 selector
balanced mux：深度较好，但需明确选择编码
```

不能仅在 Emitter 中把 priority mux 替换成 one-hot mux，因为多个 bit 同时为 1 时行为会改变。正确做法是给 IR 增加 `onehot/onehot0` 合同、verifier/assertion 和专门 lowering。

### 7.4 Memory inference

资源比较最容易出现数量级误判的是 memory：

```text
实现 A：保留为 1 个 BRAM/SRAM macro
实现 B：展开为数千 FF + decoder + mux
```

影响 inference 的因素包括：

- sync 还是 async read；
- read latency；
- 端口数量；
- read-during-write 是 old/new/no-change；
- byte enable；
- 是否 reset 整个 array；
- 目标器件和综合脚本。

pyCircuit 的 `pyc_sync_mem` 使用 registered read、byte strobe 和 old-data；`pyc_byte_mem` 是组合读 byte array。必须让对照设计具有相同 memory 合同，再使用相同 target flow 比较。

### 7.5 通常不是直接面积主因的内容

以下内容会让生成 Verilog 显得冗长，但通常会在 Yosys 中消失：

- 每个 SSA result 一个 wire；
- 多层 alias assign；
- vector packed/unpacked 的固定切片连接；
- 注释和长信号名；
- 被 `` `ifndef SYNTHESIS `` 包围的 assertion/debug 代码；
- `pyc_primitives.v` 中未被 top 实例化的其他 primitive。

它们可能影响可读性、编译时间或极端情况下的 pattern recognition，但不能仅凭 RTL 行数认定为面积根因。


## 10. 最终理解框架

可以把 pyCircuit 的 Verilog 生成看成四层：

```text
第一层：硬件意图
Python DSL 表达状态、运算、周期和层级

第二层：硬件语义
PYC dialect + verifier/pass 决定什么结构合法、如何优化

第三层：RTL 表达
VerilogEmitter 把 op 翻译为 assign、module instance 和 state primitive

第四层：技术实现
Yosys/FPGA/ASIC 工具把 RTL 映射成 DFF、LUT、BRAM、DSP 或标准单元
```

资源问题也应逐层回答：

```text
为什么需要这份硬件？       看 DSL/微结构
为什么 IR 中有这些 op？     看 frontend 和 MLIR passes
为什么 RTL 写成这个模式？  看 Emitter 和 primitives
为什么映射成这些资源？      看 Yosys/目标 flow
```

Counter 给出了最小而完整的例子：源码中的一个状态经过 cycle-aware 对齐，在已提交生成快照中成为两个 8-bit registers；Emitter 将它们机械地发射成两个 `pyc_reg`；Verilator负责判断周期行为是否正确；Yosys负责判断两个 state bank 最终是否保留、如何映射。把这条分析方法扩展到 RegisterFile、Bypass 和 Memory，就是后续资源优化工作的主路线。

## 11. 关键代码入口

| 主题 | 入口 |
|---|---|
| Emitter 前 pass pipeline | [`pycc.cpp:2254`](../compiler/mlir/tools/pycc.cpp#L2254) |
| Verilog module 发射 | [`VerilogEmitter.cpp:888`](../compiler/mlir/lib/Emit/VerilogEmitter.cpp#L888) |
| 组合拓扑排序 | [`VerilogEmitter.cpp:788`](../compiler/mlir/lib/Emit/VerilogEmitter.cpp#L788) |
| Verilog register primitive | [`runtime/verilog/pyc_reg.v`](../runtime/verilog/pyc_reg.v) |
| cycle alignment | [`v5.py:308`](../compiler/frontend/pycircuit/v5.py#L308) |
| Counter 源码 | [`counter.py:13`](../designs/examples/counter/counter.py#L13) |
| Counter 生成 Verilog | [`counter.v`](../designs/examples/counter/build/verilog/counter.v) |
| Counter testbench | [`tb_counter.py:16`](../designs/examples/counter/tb_counter.py#L16) |
| Verilator/Yosys 工程编排 | [`cli.py::_cmd_build`](../compiler/frontend/pycircuit/cli.py#L2172) |
| C++ SimObject 发射 | [`CppEmitter.cpp:867`](../compiler/mlir/lib/Emit/CppEmitter.cpp#L867) |
| MLIR op 语义/verifier | [`PYCOps.cpp`](../compiler/mlir/lib/Dialect/PYC/PYCOps.cpp) |

如需深入某个局部实现，再查阅同目录下的细分报告；日常理解、汇报和制定优化路线，以本文为主即可。
