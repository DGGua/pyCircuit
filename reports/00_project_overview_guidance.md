# pyCircuit 与 RTL 工具链入门总览

> 目标读者：学过编译原理，接触过 Verilog/Chisel，但还没有把“硬件描述、IR、仿真、综合、Emitter、PPA”串成完整心智模型的开发者。

## 0. 先建立一张正确的总图

当前 pyc4.0 的主流程并不是“把普通 C++ 程序翻译成 Verilog”。按照仓库 [README](../README.md#pycircuit-pyc40--pyc040)，它是：

```text
Python hardware-construction DSL
              │ elaboration / JIT
              ▼
       PYC dialect on MLIR
              │ legality gates + optimization passes
              ├────────────────────────┐
              ▼                        ▼
       C++ simulation model       Verilog RTL
              │                        ├── Verilator：RTL 仿真
              │                        └── Yosys/Vivado：综合
              ▼
       native C++ simulator
```

所以这里的 C++ 是一个**生成的仿真 backend**。Python 和 Chisel 中的 Scala 类似，主要用作构造硬件的宿主语言；真正被两种 backend 共同消费的语义核心是 MLIR `pyc` dialect。

这也是 Decision 0112 的含义：不能让 C++ 模型有一种行为、Verilog 又有另一种行为；语义必须先在 IR 中定义，再由两个 Emitter 分别实现。

## 1. 最重要的观念：你写的不是“按顺序运行的软件”

### 1.1 软件赋值和硬件连线不是一回事

普通 Python/C++：

```python
y = a + b
```

通常表示 CPU 执行到这里时计算一次加法并把结果放进变量。硬件 DSL 中同样的写法通常表示：

```text
a ─┐
   ├─ [加法器] ── y
b ─┘
```

只要电路上电，这个加法器就一直存在；`a` 或 `b` 改变，组合逻辑经过传播延迟后，`y` 随之改变。Python 只在 elaboration/构图阶段执行或被 JIT 分析一次。

### 1.2 循环经常表示“复制硬件”，不是“复用时间”

```python
for i in range(8):
    out[i] = a[i] + b[i]
```

若循环边界是编译期常数，它通常生成 8 个并行加法器，而不是一个加法器运行 8 次。项目中的 `LowerSCFToPYCStaticPass` 会把静态 `scf.for` 完全展开，见 [IR_SPEC](../docs/IR_SPEC.md#4-structured-control-flow-frontend-temporary-ir)。

这对资源分析非常关键：软件源码只有几行，不代表硬件面积小。循环次数、vector lane 数、展开策略都会直接乘上硬件数量。

### 1.3 `if` 经常表示 mux

硬件条件：

```python
y = mux(sel, a, b)
```

对应：

```text
       ┌── a
sel ─ [2:1 MUX] ─ y
       └── b
```

JIT 路径中的硬件 `if` 也会先表示为 `scf.if`，再静态 lowering 成 mux 网络。两个分支的硬件往往都存在；它不是 CPU 只执行一个分支的意思。

## 2. RTL 到底是什么

RTL 是 Register Transfer Level，最实用的理解是：

> 描述“寄存器里当前保存什么状态”，以及“每个时钟沿到来时，状态由哪些组合逻辑计算出的 next value 更新”。

一个带 enable 的 8-bit counter 可写成状态方程：

```text
next_count = enable ? count + 1 : count

posedge clk:
    if reset: count := 0
    else:     count := next_count
```

它由两部分组成：

```text
                  ┌───────────────┐
count ──► +1 ──►  │ enable mux    │ ──► next_count
  │               └───────────────┘
  │                                      │
  └──────────── [8-bit register] ◄───────┘
                         ▲
                      clk/reset
```

组合逻辑没有跨周期记忆；register/memory/FIFO 才保存状态。寄存器把组合路径切开，也是周期边界和 timing path 的起点/终点。

### 2.1 常见基础术语

| 术语 | 直观含义 | 在 pyCircuit 中常见形态 |
|---|---|---|
| signal | 一组同时存在的电气值/bit-vector | `Wire`、`Signal`、`iN` |
| combinational logic | 输出只依赖“当前输入”，不保存历史 | `pyc.add`、`pyc.mux`、`assign` |
| state/sequential logic | 在时钟沿更新并跨周期保存 | `pyc.reg`、FIFO、memory |
| clock | 规定 state 何时一起采样/更新 | `!pyc.clock`、`clk` |
| reset | 把规定的 state 带回已定义状态 | `!pyc.reset`、`init` |
| cycle | 相邻有效时钟沿之间的逻辑时间单位 | `CycleAwareDomain` occurrence |
| latency | 输入到对应输出要经过多少拍 | 自动/手工 pipeline 的级数 |
| throughput | 稳态每拍能接受/产生多少项 | 可能是 1 item/cycle，即使 latency 很大 |
| combinational depth | 一个周期内串联了多少级逻辑 | `CheckLogicDepthPass` 的 proxy |
| critical path | 最慢的寄存器到寄存器组合路径 | 最终需 STA/目标技术确认 |
| hierarchy | module 与 instance 的包含关系 | `@module`、`pyc.instance` |
| CDC | 信号跨 clock domain | `pyc.cdc_sync`、`pyc.async_fifo` |

### 2.2 `wire`、`reg` 和真实硬件资源

不要简单按 Verilog 关键词数资源：

- `wire`/`assign` 往往只是连接关系，综合后可能完全消失；
- `pyc_reg` 或时钟 `always` 通常代表真实 flip-flop bank；
- `a + b` 可能映射为 carry chain/adder cells；
- 大 mux、比较器、动态 shift 会消耗大量逻辑；
- memory array 能否保留为 BRAM/SRAM macro，常常决定数量级。

传统 Verilog 中 `reg` 也只是过程赋值对象的语言类型，不保证一定产生寄存器；是否产生硬件 state 取决于 `always` 的时序写法。pyCircuit 则用显式 `pyc.reg` 和 `pyc_reg` primitive 表达 state，概念更直接。

### 2.3 组合反馈和时序反馈

合法 counter 是：

```text
q ─► combinational next logic ─► DFF ─► q
```

DFF 切断了环。若没有 DFF：

```text
x ─► NOT ─► x
```

就是组合环，可能振荡或无法稳定。项目的 `CheckCombCyclesPass` 在 MLIR 层拒绝它，不能等 Yosys 或仿真器碰巧处理。

### 2.4 Module、instance、hierarchy 与 flatten

`module` 像“硬件类/模板”，`instance` 像把模板真正放进设计的一份硬件对象：

```verilog
module Adder(...); ... endmodule

Adder u0(...);  // 第一份硬件
Adder u1(...);  // 第二份硬件，不是对 u0 的软件函数调用
```

两个 instance 通常代表两份资源。hierarchy 保存 parent/child 边界，便于增量编译、debug 和独立综合；flatten 则像把被调用函数体内联进 top，可能帮助跨模块常量传播和逻辑共享，也可能让编译规模暴涨。pyCircuit 用 `@module → func.func` 定义模板，用 `pyc.instance` 表示实例。

### 2.5 Ready/valid 是一次“双方同意的传输”

FIFO 和流水接口中常见：

```text
producer: valid + data ─────► consumer
producer ◄──────────── ready: consumer

transfer = valid && ready
```

`valid=1` 表示发送方的数据有效，`ready=1` 表示接收方本拍能接收；只有二者同时为 1 才发生一次 transfer。单独看到 `valid` 不能认为数据已被消费。FIFO、backpressure 和跨模块 observation point 都依赖这个合同。

### 2.6 看生成 Verilog 时最常遇到的三种语句

```verilog
assign y = a + b;             // continuous：组合逻辑一直驱动 y

always @(*) y = sel ? a : b;  // procedural combinational logic

always @(posedge clk) begin   // sequential logic
  if (rst) q <= 0;
  else     q <= d;
end
```

时钟块里的 non-blocking assignment `<=` 表示所有寄存器先采样旧状态，再统一更新；这正对应 C++ backend 的 `tick_compute()` / `tick_commit()` 两阶段。若错误地按普通 C++ 语句从上到下立即修改 q，会制造寄存器更新竞态。

## 3. elaboration、编译、仿真、综合分别做什么

先用编译器术语快速对照：

| 术语 | 在本项目中的含义 |
|---|---|
| frontend | Python API/JIT → PYC MLIR |
| IR/dialect | backend-neutral 的硬件操作和类型集合 |
| verifier/gate | 检查 IR 是否满足硬件合同，不满足则拒绝编译 |
| pass | 在 IR 上分析或改写的编译阶段 |
| backend | C++ simulation 或 Verilog 目标 |
| Emitter | 把合法 IR 写成目标语言/产物的 code generator |
| runtime | 生成代码依赖的 register/memory/TB 等支撑实现 |
| testbench | 产生 clock/reset/input 并检查 output 的非综合环境 |
| netlist | module/cell 与 wire 的结构图；可以是 generic 或 technology-mapped |
| technology mapping | 把 `$add/$mux/$dff` 等 generic cell 映射到具体 FPGA/ASIC cell |
| PPA | Power、Performance、Area；必须说明目标技术和测量阶段 |

### 3.1 Elaboration：把宿主语言程序变成一张确定的硬件图

Chisel 会执行 Scala generator；pyCircuit 会执行或 JIT 分析 Python design function。参数、Python list、静态循环、helper 调用在这里决定：

- 有几个模块实例；
- vector 有多少 lane；
- 寄存器和 memory 的宽度/深度；
- 哪些 Python 结构只是生成期数据，哪些是硬件 signal。

pyCircuit 的底层构图在 [dsl.py](../compiler/frontend/pycircuit/dsl.py#L200)，高层硬件对象在 [hw.py](../compiler/frontend/pycircuit/hw.py#L650)，多模块编译单元在 [design.py](../compiler/frontend/pycircuit/design.py#L339)。

### 3.2 Compiler passes：在 IR 上检查、规范化和优化

MLIR 类比普通编译器的中间表示，但它的 op 是硬件语义：

```mlir
%sum = pyc.add %a, %b : i8, i8 -> i8
%q   = pyc.reg %clk, %rst, %en, %next, %init : i8
```

pass 的作用类似 LLVM optimization/verification：

- canonicalize/CSE：常量折叠、公共表达式消除；
- lower static control flow：把生成期/临时结构变成静态电路；
- dead-state/wire elimination：删除不可观察结构；
- vector unroll/SLP：在 vector 与 scalar lanes 之间转换；
- comb-cycle/clock-domain/type/depth gate：拒绝不合法或超约束硬件。

固定 pipeline 的真实入口在 [pycc.cpp:2254](../compiler/mlir/tools/pycc.cpp#L2254)，概览见 [PIPELINE](../docs/PIPELINE.md)。

### 3.3 Emitter：把已确定的 IR 翻译成目标语言

Emitter 就是 compiler backend 中的 code generator：

- [VerilogEmitter.cpp](../compiler/mlir/lib/Emit/VerilogEmitter.cpp) 把 `pyc.add` 写成 Verilog `assign ... + ...`，把 `pyc.reg` 写成 `pyc_reg` instance；
- [CppEmitter.cpp](../compiler/mlir/lib/Emit/CppEmitter.cpp#L867) 把同一个 op 生成 C++ 数据结构和求值函数。

理想情况下，Emitter 不再猜测算法和语义，只忠实地把已经合法化的 IR lowering 到目标语言。因此“发现硬件结构太大”时，要先确认结构在哪一层首次出现，不能默认问题就在 Emitter。

### 3.4 Simulation：给时间和输入，观察设计行为

仿真不会把电路变成 FPGA/芯片。它做的是：

1. testbench 在某个周期 drive 输入；
2. 组合逻辑求值；
3. 时钟沿触发 state update；
4. 在规定 observation point 检查输出；
5. 可选生成 VCD/trace。

项目有两条互相校验的路径：

| 路径 | 输入 | 执行者 | 价值 |
|---|---|---|---|
| C++ simulation | MLIR → C++ Emitter | native C++ runtime | 大设计快速、可控的功能仿真 |
| RTL simulation | MLIR → Verilog Emitter | Verilator | 验证生成 RTL 的真实行为 |

C++ runtime 的 phase 是 comb/eval、tick_compute、tick_commit，见 [架构文档](../docs/v6_PyCircuit_Software_Architecture.md#7-c-发射器与仿真模型)。Verilator 构建命令由 [cli.py:2508](../compiler/frontend/pycircuit/cli.py#L2508) 组织。

仿真正确不等于综合资源好；资源好也不等于功能正确。二者是两条不同 gate。

### 3.5 Synthesis：把 RTL 变成逻辑网络

Yosys 综合大致经历：

```text
Verilog text
  │ read_verilog / hierarchy
  ▼
process lowering（always/case → mux、DFF 等内部 cell）
  │ opt
  ▼
memory recognition / collection / mapping
  │ generic synth
  ▼
generic logic network（$mux、$add、$dff、$mem...）
  │ ABC / techmap / dfflibmap
  ▼
FPGA LUT/FF/BRAM/DSP 或 ASIC standard cells/macros
```

当前 `pycc` 自动生成的脚本只有 sanity synthesis，见 [pycc.cpp:2487](../compiler/mlir/tools/pycc.cpp#L2487)。详细分析方法见 [02_yosys_verilator_flow](02_yosys_verilator_flow.md)。

### 3.6 Place & Route / STA：综合之后还有一大段

综合给出逻辑 cell/netlist，但真实频率、线长、拥塞、功耗还要依赖：

- FPGA place & route（Vivado/Quartus 等）；
- ASIC floorplan/place/clock-tree/route；
- STA（Static Timing Analysis）；
- 目标器件、时钟约束和工艺库。

`CheckLogicDepthPass` 只是编译期结构 proxy，不是 STA。`compile_stats.wns` 也不是工艺库下真正的 WNS。

## 4. 用 Chisel 做对应理解

| Chisel/CIRCT 世界 | pyCircuit 世界 | 含义 |
|---|---|---|
| Scala host language | Python host language | 用普通语言生成硬件 |
| `Module` | `@module` / `func.func` | 可实例化的层级边界 |
| `Wire(UInt(8.W))` | `Wire` / `i8` | 组合信号 |
| `RegInit(0.U)` | `domain.signal(reset_value=0)` / `pyc.reg` | 带初值/复位的状态 |
| `Vec(n, T)` | `Vec` / `vector<nxiW>` | 多 lane 数据 |
| `Mux(sel,a,b)` | `mux` / `pyc.mux` | 2:1 选择 |
| FIRRTL/CIRCT IR | PYC MLIR dialect | backend-neutral 硬件 IR |
| lowering passes | `compiler/mlir/lib/Transforms/` | 合法化、优化、分析 |
| SystemVerilog emitter | `VerilogEmitter.cpp` | 输出 RTL 文本 |
| Verilator backend | Verilator path | RTL 仿真 |

公平比较应尽量在同一语义、同一 top、同一综合脚本的 post-synthesis netlist 处进行，而不是比较 Scala/Python 行数或生成 Verilog 行数。

## 5. pyCircuit 仓库代码地图

### 5.1 第一级：你写设计时直接接触的 frontend

| 文件/目录 | 职责 | 出问题时的典型症状 |
|---|---|---|
| [`compiler/frontend/pycircuit/dsl.py`](../compiler/frontend/pycircuit/dsl.py) | 最底层 `Signal` 和 MLIR 文本构造；每个 API 最终发出 `pyc.*` op | op 类型/宽度或原始 MLIR 不对 |
| [`hw.py`](../compiler/frontend/pycircuit/hw.py) | `Circuit/Wire/Reg/Vec`，运算符重载、state、memory、instance API | 一个 Python 表达式生成了意外的 mux/vector/寄存器 |
| [`v5.py`](../compiler/frontend/pycircuit/v5.py) | `CycleAwareDomain/Signal`、周期传播、自动 `delay_to()` | `_v5_bal_*` 太多、latency/周期不对 |
| [`design.py`](../compiler/frontend/pycircuit/design.py) | `@module/@function` 与多 module `Design` | hierarchy、specialization、module symbol 不对 |
| [`jit.py`](../compiler/frontend/pycircuit/jit.py) | Python AST JIT；处理硬件 if、静态 for、调用 | Python 语法无法编译或 control flow lowering 异常 |
| [`connectors.py`](../compiler/frontend/pycircuit/connectors.py) / `record.py` / `spec/` | 结构化接口、Bundle/Record、连接 | port flatten、字段映射、跨模块连接异常 |
| [`cli.py`](../compiler/frontend/pycircuit/cli.py) | `emit/build` 编排、TB SV/C++ 生成、Verilator 调用 | 构建产物、testbench、缓存、命令行问题 |

可以把 `dsl.py` 看作 AST builder，把 `hw.py/v5.py` 看作语言前端和类型/时序语义层，把 `design.py/jit.py` 看作 elaborator。

### 5.2 第二级：硬件 IR 的“语言定义”

| 文件 | 职责 |
|---|---|
| [`PYCOps.td`](../compiler/mlir/include/pyc/Dialect/PYC/PYCOps.td) | TableGen 声明 op、operand/result、trait |
| [`PYCTypes.td`](../compiler/mlir/include/pyc/Dialect/PYC/PYCTypes.td) | clock/reset 等 dialect types |
| [`PYCOps.cpp`](../compiler/mlir/lib/Dialect/PYC/PYCOps.cpp) | verifier、fold/canonicalization 等 op 实现 |
| [`IR_SPEC.md`](../docs/IR_SPEC.md) | 人读的 op 语义说明 |

若问题是“除零到底返回什么”“register reset 怎么定义”“memory read-during-write 是 old 还是 new data”，应从这里和 decisions 入手，而不是先看 Verilog 写法。

### 5.3 第三级：passes 与 gates

目录 [`compiler/mlir/lib/Transforms/`](../compiler/mlir/lib/Transforms/) 中：

- `Check*Pass.cpp`：合法性门禁；
- `CombDepGraph.cpp`：组合依赖图，服务 comb loop/depth；
- `LowerSCFToPYCStaticPass.cpp`：动态样式的临时 IR → 静态硬件；
- `Eliminate*Pass.cpp`：删除 wire、dead state、dead instance；
- `CombCanonicalizePass.cpp`：组合模式化简；
- `SLPPackWiresPass.cpp` / `VectorUnrollPass.cpp`：vector/scalar 结构变换；
- `PackI1RegsPass.cpp`：保守打包相邻 1-bit register；
- `FuseCombPass.cpp`：把组合 op 融合为 codegen region；
- `CollectCompileStatsPass.cpp`：收集 IR inventory，不是综合面积。

项目 ground rule 是 gate-first：若一个优化需要新的前置条件，例如 selector 必须 one-hot，应先让 IR 能表达并验证该合同，再写 rewrite。

### 5.4 第四级：backend driver 与 Emitters

| 文件 | 职责 |
|---|---|
| [`compiler/mlir/tools/pycc.cpp`](../compiler/mlir/tools/pycc.cpp#L2015) | 读 `.pyc`、配置 pass pipeline、选择 backend、写 manifest/stats/Yosys stub |
| [`VerilogEmitter.cpp`](../compiler/mlir/lib/Emit/VerilogEmitter.cpp#L888) | 每个 `func.func` → Verilog module |
| [`CppEmitter.cpp`](../compiler/mlir/lib/Emit/CppEmitter.cpp#L867) | 每个 module → C++ simulator structure/eval/tick/commit |
| [`pyc-opt`](../compiler/mlir/tools/pyc-opt.cpp) | 单独运行 pass、调试 IR 和写 FileCheck regression |

`pycc` 类似 `clang` driver；Emitter 类似 LLVM 的 instruction selection/assembly printer，只是目标是 Verilog 或 C++。

### 5.5 第五级：runtime primitives

| 目录 | 内容 |
|---|---|
| [`runtime/verilog/`](../runtime/verilog/) | `pyc_reg`、FIFO、memory、CDC 的 Verilog 实现 |
| [`runtime/cpp/`](../runtime/cpp/) | 同类 primitive 的 C++ 模型、Bits/Vec、testbench、VCD、trace |

例如 Emitter 不为每个 register 手写 `always`，而是实例化 [pyc_reg.v](../runtime/verilog/pyc_reg.v)。若 register 的 reset/enable 结构影响综合，这个 primitive 很重要；但改变它的语义必须同步修改 C++ runtime 并通过 equivalence gate。

### 5.6 第六级：designs、tests、flows

| 路径 | 用途 |
|---|---|
| [`designs/examples/`](../designs/examples/) | 最小到中型入门设计 |
| [`designs/BypassUnit/`](../designs/BypassUnit/) | 自动周期平衡、vector/mux、资源研究的好 case |
| [`designs/RegisterFile/`](../designs/RegisterFile/) | state array、multiport read/write 结构研究 |
| [`tests/vec/`](../tests/vec/) | 生成式 vector op、C++/Verilator 和少量 Yosys smoke |
| [`flows/scripts/`](../flows/scripts/) | 构建 toolchain、运行 examples/sims |
| [`docs/gates/`](../docs/gates/) | pyc4.0 gate 状态与日志 |

## 6. Counter：从 Python 一路看到硬件

仓库示例在 [counter.py](../designs/examples/counter/counter.py#L13)：

```python
enable = cas(domain, m.input("enable", width=1), cycle=0)
count = domain.signal(width=width, reset_value=0, name="count")
m.output("count", wire_of(count))

domain.next()
count.assign(count + 1, when=enable)
```

可以直接查看 eager frontend 的 raw MLIR：

```bash
PYTHONPATH=compiler/frontend python3 designs/examples/counter/counter.py
```

### 6.1 Frontend 生成的 raw MLIR

当前源码生成的关键部分如下；为便于阅读，这里把实际的 `%vN` SSA 名改成了语义名字：

```mlir
%count_next = pyc.wire : i8
%count_q = pyc.reg %clk, %rst, %one, %count_next, %zero : i8

%balance_next = pyc.wire : i8
%balance_q = pyc.reg %clk, %rst, %one, %balance_next, %zero : i8
pyc.assign %balance_next, %count_q : i8

%inc = pyc.add %balance_q, %one8 : i8, i8 -> i8
%next = pyc.mux %enable, %inc, %count_q : i1, i8, i8 -> i8
pyc.assign %count_next, %next : i8
```

对有编译基础的人，`pyc.wire + pyc.assign` 可以理解为解决 SSA backedge 的占位：register 的 `next` 逻辑在源码顺序上稍后才构造，但 SSA value 通常要求定义先于使用。

值得注意的是，这里不是一个而是两个 8-bit `pyc.reg`。原因是 `domain.next()` 把赋值表达式放到后一个逻辑 occurrence；读取较早的 `count` 时，cycle-aware frontend 用 `delay_to()` 插入 `_v5_bal_1` 对齐。对应实现见 [v5.py:308](../compiler/frontend/pycircuit/v5.py#L308)。

这正是资源分析中应养成的习惯：不要看到 Python 里只有一个 `count` 就认定只有 8 个 FF；先看 raw/pass 后 IR。该 balance stage 是否为语义必需，需要结合 cycle contract 和仿真判断，不能在 Emitter 里直接删除。

### 6.2 生成的 Verilog

已提交产物在 [counter.v](../designs/examples/counter/build/verilog/counter.v)。它包含：

```verilog
assign pyc_add_11 = (_v5_bal_1 + 8'd1);
assign pyc_mux_12 = (enable ? pyc_add_11 : count_2);

pyc_reg #(.WIDTH(8)) pyc_reg_10_inst (...);
pyc_reg #(.WIDTH(8)) pyc_reg_7_inst (...);

assign count = count_2;
```

中间有很多 `wire` 和 alias，文本看起来比手写 counter 长很多；但资源上首先应关注两个 `pyc_reg`、一个 adder 和一个 mux。当前 [compile_stats.json](../designs/examples/counter/build/verilog/compile_stats.json) 也记录了 2 个 register、16 bits、logic depth 2。

Yosys 很可能清理大部分 alias/常量 wire，但它不会无条件删掉影响逐周期行为的第二个 register。

### 6.3 Testbench 与仿真

[tb_counter.py](../designs/examples/counter/tb_counter.py#L16) 声明 clock/reset，然后按周期 drive `enable` 并 expect `count`。canonical 命令是：

```bash
PYTHONPATH=compiler/frontend \
PYC_TOOLCHAIN_ROOT=.pycircuit_out/toolchain/install \
python3 -m pycircuit.cli build \
  designs/examples/counter/tb_counter.py \
  --out-dir /tmp/pyc_counter \
  --target both \
  --run-verilator
```

`--target both` 会从同一设计构建 C++ simulator 和 Verilog/Verilator 两个版本；`--run-verilator` 只自动运行 Verilator binary。C++ executable 路径记录在 `project_manifest.json` 的 `cpp_executable`，还需单独运行。两个 binary 都在相同 observation points 通过，才说明 backend 语义一致。

### 6.4 Yosys 综合

示例自动生成的 [yosys_synth.ys](../designs/examples/counter/build/verilog/yosys_synth.ys) 只做：

```tcl
read_verilog -sv pyc_primitives.v
read_verilog -sv counter.v
hierarchy -top counter
proc; opt; memory; opt
synth -top counter
```

它能回答“RTL 能否被 Yosys 综合”，但没有目标 FPGA/Liberty，也没有 JSON resource report。不要把 `compile_stats.json` 当成 Yosys 结果。

## 7. 一个 op 如何穿过整条链路

以 `a + b` 为例：

```text
Python: Wire.__add__
  ▼
hw.py 调用 dsl.py builder
  ▼
raw IR: %r = pyc.add %a, %b
  ▼
PYCOps verifier/fold + canonicalize/CSE
  ▼
VerilogEmitter: assign r = a + b
  ├─ Verilator：执行 RTL 加法语义
  └─ Yosys：转成 $add，再映射为 carry/LUT/cells

同一个 optimized IR
  ▼
CppEmitter: 生成 C++ Bits 加法调用
  ▼
C++ runtime：仿真时计算结果
```

排查加法语义 bug 时沿纵向追踪这一条链；排查加法器太多时则横向查“是谁在 frontend/IR 中复制了这么多 `pyc.add`”。

## 8. 资源分析时该看什么

### 8.1 RTL 文本层容易产生的错觉

| 现象 | 是否通常代表面积 |
|---|---|
| 文件行数很大 | 不一定；生成代码常有大量命名和连接 |
| `wire` 很多 | 不一定；多数会被 opt 清理 |
| `assign` 很多 | 不一定；可能只是 slice/alias，也可能是真逻辑 |
| `pyc_reg` 很多/很宽 | 高度相关；是 state bits |
| 大量 compare + mux chain | 高度相关；尤其多端口 register file |
| memory 变成逐 entry registers | 高风险，可能是数量级差异 |
| reset 接到每个 data register | 高风险，会影响 cell 和 FPGA inference |

### 8.2 必须分层取数

建议为同一 case 保留：

1. raw MLIR：frontend 生成了什么；
2. optimized MLIR：pycc passes 删除/改写了什么；
3. emitted Verilog：primitive/层级是否符合预期；
4. Yosys post-proc：RTL process 变成了哪些 generic cells；
5. post-memory：memory 是否仍被识别；
6. generic synth：逻辑规模；
7. target map：LUT/FF/BRAM/DSP 或 standard-cell area；
8. P&R/STA：真实频率、拥塞、功耗。

差异第一次在哪一层出现，通常就决定该找谁。

## 9. 按问题找代码

| 你看到的问题 | 第一站 | 第二站 |
|---|---|---|
| Python 表达式生成了错误 op/宽度 | `hw.py` / `v5.py` | `dsl.py` / raw MLIR |
| 静态循环或硬件 if 不对 | `jit.py` | `LowerSCFToPYCStaticPass.cpp` |
| 自动多出 pipeline register | `v5.py::delay_to` | raw MLIR、cycle metadata |
| pass 后逻辑突然变大/变小 | 对应 `Transforms/*.cpp` | `pyc-opt` 单 pass reproducer |
| 多 driver、comb loop、CDC | `PYCOps.cpp` + `Check*Pass.cpp` | decisions 0130/0134 等 |
| Verilog 运算符/端口/实例名错误 | `VerilogEmitter.cpp` | pass 后 MLIR |
| register/FIFO/memory RTL 结构差 | `runtime/verilog/pyc_*.v` | 对应 dialect 语义和 C++ primitive |
| C++ 与 Verilog 结果不同 | 两个 Emitter + observation point | runtime primitives / TB sampling |
| Verilator 编译或 TB 采样问题 | `cli.py::_render_tb_sv` | generated TB、manifest |
| Yosys 资源异常 | post-pass IR + Yosys cut points | memory/one-hot/reset/target mapping |
| `compile_stats` 与 Yosys 不一致 | `CollectCompileStatsPass.cpp` | top reachability/hierarchy |

## 10. 推荐学习顺序

### 第一阶段：能看懂一拍硬件

1. 手画 counter 的 q/next/register 图；
2. 阅读 [counter.py](../designs/examples/counter/counter.py)；
3. 对照 raw MLIR、[counter.v](../designs/examples/counter/build/verilog/counter.v) 和 [pyc_reg.v](../runtime/verilog/pyc_reg.v)；
4. 明确哪部分是 wire、组合逻辑、state、reset。

### 第二阶段：理解编译分层

1. 阅读 [PIPELINE](../docs/PIPELINE.md)；
2. 用 `pyc-opt` 单独运行一个 canonicalize/eliminate/check pass；
3. 跟踪 `Wire.__add__ → dsl.add → pyc.add → Emitter`；
4. 比较 raw 与 pass 后 IR，而不是只看最终 `.v`。

### 第三阶段：理解仿真时间

1. 阅读 [TESTBENCH](../docs/TESTBENCH.md)；
2. 跑 counter 的 C++ 与 Verilator 两个 backend；
3. 观察 drive、posedge、post-expect、reset deassert 的顺序；
4. 再阅读 C++ `eval/tick_compute/tick_commit`。

### 第四阶段：理解综合和资源

1. 对 counter 保存 Yosys post-proc 和 synth stats；
2. 手写一个语义等价 counter，用同一脚本比较；
3. 对一个 mux case 比较 true-priority 与 proven-onehot；
4. 对 memory case 观察 `$mem` 是保留还是展开；
5. 最后进入 BypassUnit 和 RegisterFile。

### 第五阶段：开始做优化

先从可证明的小任务开始：

- top-reachable IR stats；
- single-driver verifier；
- balance register provenance；
- one-hot contract + microbenchmark；
- memory inference regression。

完整顺序见 [04_optimization_roadmap](04_optimization_roadmap.md)。

## 11. 阅读代码时持续问的十个问题

1. 这段 Python 是 elaboration-time，还是表示 runtime hardware？
2. 一个循环会复制多少份硬件？
3. 这个 value 是组合信号还是跨周期 state？
4. 它属于第几拍、哪个 clock/reset domain？
5. reset 后它必须可观察地为零吗？
6. mux 是真正的 priority，还是 selector 已知 one-hot？
7. memory 的端口数、read latency、RDW 规则是什么？
8. module 是真实连接的 instance，还是只有同名定义？
9. 当前数字来自 raw IR、optimized IR、generic synth 还是 target map？
10. 这个“优化”是否同时保持 C++ 与 Verilog observation-point 等价？

能稳定回答这十个问题，就已经具备参与 pyCircuit 资源优化的核心框架。

## 12. 本指南之后的阅读顺序

1. 本文：形成领域和项目总图；
2. [01_verilog_emitter_pipeline](01_verilog_emitter_pipeline.md)：深入 Emitter 与 primitives；
3. [02_yosys_verilator_flow](02_yosys_verilator_flow.md)：建立正确的仿真/综合实验；
4. [03_resource_gap_root_causes](03_resource_gap_root_causes.md)：理解当前已发现的资源风险；
5. [04_optimization_roadmap](04_optimization_roadmap.md)：选择第一批实现任务。
