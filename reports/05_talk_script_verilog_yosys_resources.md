# pyCircuit Verilog 生成结构、资源问题与 Yosys 综合

## 内部反串讲讲稿

> 建议时长：主讲 35–45 分钟，问答 10 分钟。  
> 目标听众：了解少量 Verilog/Chisel，知道基本编译器概念，但不熟悉 pyCircuit backend 和硬件综合。  
> 内容基于 2026-08-04 当前工作树。代码中的数字和结论区分为“源码已确认”“强推断”“待统一综合验证”。

## 使用说明

本文每一节可对应一页投影片：

- **投影片**：屏幕上应保留的少量要点；
- **讲稿**：可以直接照着讲，也可以按自己的表达缩短；
- **展示代码**：建议现场打开的文件或片段；
- **我的判断**：带观点的结论，需要与客观实现分开；
- **过渡**：进入下一页时使用。

这次讲述的主线不是“逐行读 Emitter”，而是回答三个问题：

1. pyCircuit 的 Verilog 到底是如何生成的？
2. 为什么生成 RTL 看起来很大，资源也可能比 Chisel/手写多？
3. Verilator、Yosys 和 `compile_stats.json` 分别能证明什么？

---

## 第 1 页：开场——先给结论

### 投影片

```text
Verilog 大 ≠ 面积一定大
Yosys ≠ 本项目的 RTL 仿真器
资源结构通常在 Emitter 之前已经决定
优化必须 gate-first，不能 backend 偷改语义
```

### 讲稿

大家好，今天我想反串讲一下 pyCircuit 从硬件模型到生成 Verilog，再到仿真、Yosys 综合和资源分析的整条链路。

我先把结论摆出来。第一，生成的 Verilog 文件很长，不等于最终面积一定大，因为很多 `wire`、alias 和 packed/unpacked bridge 综合后只是连线。第二，在当前项目中，Verilator 才是 RTL 仿真器；Yosys 的角色是综合和结构分析。第三，真正决定寄存器数量、memory 形态和 mux 拓扑的地方，很多时候在 frontend 或 MLIR，而不是 Verilog Emitter。第四，如果优化会改变 reset、memory、priority 等语义，就必须先在 dialect 和 verifier 中建立合同，不能只在 Emitter 里做一个“更省资源”的特殊打印。

所以今天不会把所有问题都归咎于生成器，也不会拿 Verilog 行数直接推导面积。我会沿着编译链路定位：差异第一次出现在哪里。

### 我的判断

当前最值得做的不是立即承诺“面积降低百分之多少”，而是先建立可比较、可复现、可定位的资源度量流程。否则优化前后可能连设计语义、top 或 memory mapping 都不同。

### 过渡

为了后面不混淆，我先统一几个词。

---

## 第 2 页：术语校准——RTL、仿真、综合、Emitter

### 投影片

| 术语 | 一句话解释 |
|---|---|
| RTL | state register + next-state combinational logic |
| 仿真 | 给定输入和时间，计算逐周期行为 |
| 综合 | 把 RTL 转成 generic/target cell netlist |
| Emitter | 把合法 IR 翻译成目标语言 |
| PPA | 功耗、性能、面积；依赖目标技术与约束 |

### 讲稿

RTL 是 Register Transfer Level。直观上，它描述寄存器当前保存什么，以及下一个时钟沿时，组合逻辑算出的 next value 如何写回寄存器。

仿真是在时间轴上运行设计。testbench drive 输入，时钟沿更新 state，然后在规定的 observation point 检查输出。仿真不会告诉我们最终用了多少 LUT 或标准单元。

综合则不运行 testbench。它把 Verilog 的 `assign`、`always`、memory pattern 识别成 `$mux`、`$add`、`$dff`、`$mem` 等内部 cell，再映射到 FPGA LUT/FF/BRAM/DSP 或 ASIC 标准单元和 macro。

Emitter 可以类比普通编译器的 code generator。它消费已经合法化的 IR，把 `pyc.add` 写成 Verilog 加法，把 `pyc.reg` 写成一个 `pyc_reg` 实例。它不应该自己重新发明硬件语义。

最后是 PPA。没有固定器件、Liberty、clock constraint、flatten 策略和 memory macro policy，就没有可比较的 PPA。`compile_stats.json` 里的 WNS 也只是 logic-depth proxy，不是真正 STA 的 WNS。

### 可补充的类比

```text
LLVM IR → x86 instruction selection → assembly
PYC MLIR → VerilogEmitter          → Verilog
PYC MLIR → CppEmitter              → C++ simulation model
```

### 过渡

有了这些词，下面看当前项目真正的数据流。

---

## 第 3 页：项目总数据流——当前主输入不是 C++

### 投影片

```text
Python hardware DSL
       │ elaboration / JIT
       ▼
    PYC MLIR
       │ pycc passes + legality gates
       ├──────────────┐
       ▼              ▼
 C++ simulator    Verilog RTL
                      ├─ Verilator simulation
                      └─ Yosys/Vivado synthesis
```

### 讲稿

这里需要澄清一个容易产生的历史印象。当前 pyc4.0 README 定义的主流程，是 Python hardware-construction DSL 编译到 PYC MLIR，然后从同一份优化后 IR 生成 C++ 仿真模型和 Verilog RTL。

所以 C++ 在当前主路径里是生成的仿真 backend，不是普通 C++ 算法代码直接翻译成硬件。Python 的角色更像 Chisel 中的 Scala：宿主语言负责参数化和构图；真正的 backend-neutral 语义中心是 PYC dialect。

这件事非常重要，因为 C++ simulator 与 Verilog 要从同一 IR 得到逻辑等价的结果。项目 Decision 0112 明确规定 MLIR 是单一语义来源。reset/init、memory read-during-write、观察点和 net/var 也必须在 dialect 层稳定。

### 展示代码

- 项目定位：[README.md](../README.md#pycircuit-pyc40--pyc040)
- 两阶段 pipeline：[docs/PIPELINE.md](../docs/PIPELINE.md)
- 单一 IR 双发射：[pyc4.0 Decision 0112](../docs/rfcs/pyc4.0-decisions.md#decision-0112-mlir-dialect-is-the-single-semantic-source-c-sim-and-verilog-emission-must-be-logically-equivalent)

### 过渡

那么 Python 是怎样变成硬件图的？这需要区分 elaboration 和 runtime。

---

## 第 4 页：硬件 DSL 的关键——代码不是按软件方式运行

### 投影片

```python
for i in range(8):
    out[i] = a[i] + b[i]
```

```text
通常是 8 个并行加法器，不是 1 个加法器运行 8 次
```

### 讲稿

在普通 Python 里，`a + b` 是程序运行到这里时计算一次。在硬件 DSL 中，它通常意味着构造一个一直存在的加法器。输入变化，组合输出随之变化。

同样，一个静态 `for range(8)` 往往复制 8 份硬件，而不是做 8 个软件时间步。硬件 `if` 往往生成 mux，两个分支的组合电路都存在。pyCircuit 的 JIT 可以暂时生成 `scf.if` 和 `scf.for`，但 `LowerSCFToPYCStaticPass` 会把它们 lowering 成静态 mux 或完全展开的硬件。

这解释了为什么源码只有十几行，也可能生成几千个 compare 和 mux。面积与 elaborated graph 相关，不与 Python 行数成正比。

### 展示代码

- 底层 MLIR builder：[dsl.py:200](../compiler/frontend/pycircuit/dsl.py#L200)
- `Wire` 运算符与禁止 Python bool：[hw.py:98](../compiler/frontend/pycircuit/hw.py#L98)
- 静态控制流合同：[IR_SPEC.md:201](../docs/IR_SPEC.md#L201)

### 我的判断

做资源优化的人首先要学会“用硬件数量读 Python”：循环乘了多少实例、vector 有多少 lane、一个 helper 是 inline 逻辑还是 module instance、一次 `domain.next()` 引入多少状态。

### 过渡

Frontend 构好图以后，`pycc` 不会直接打印 Verilog，它先跑一整套 pass 和门禁。

---

## 第 5 页：`pycc` 的位置——Emitter 前还有完整 pipeline

### 投影片

```text
frontend contract / hierarchy
canonicalize + CSE + SCCP
SCF static lowering
wire/dead-state elimination
vector unroll or SLP pack
comb canonicalization
comb-cycle / CDC / type / depth gates
i1 reg packing / comb fusion
stats
→ Emitter
```

### 讲稿

`pycc` 是后端 driver，类似 `clang`。它读 `.pyc` MLIR，配置固定 pass pipeline，最后根据 `--emit` 选择 C++ 或 Verilog Emitter。

当前 pass 顺序在 `pycc.cpp` 2254 行附近。先检查 frontend contract 和 hierarchy，然后做 canonicalize、CSE、SCCP，lower 静态控制流，消除 wire 和 dead state。vector 可以 unroll，或者反过来把同构 scalar lanes 做 SLP packing。后面还有 comb canonicalization、组合环、clock domain、flat type、no-dynamic 和 logic-depth gate，最后收集 compile stats。

这里有两个重要结论。第一，Emitter 看到的已经不是原始 Python 结构，而是 pass 后 IR。第二，如果资源突然增大，必须同时保存 raw MLIR 和 optimized MLIR，判断是 frontend 已经生成了，还是某个 pass 改写出来的。

### 展示代码

- 固定 pipeline：[pycc.cpp:2254](../compiler/mlir/tools/pycc.cpp#L2254)
- pass 文件目录：[compiler/mlir/lib/Transforms](../compiler/mlir/lib/Transforms/)
- op 定义：[PYCOps.td](../compiler/mlir/include/pyc/Dialect/PYC/PYCOps.td)

### 我的判断

资源优化最值得建设的调试能力，是给同一个 case 保存 pass 前后结构统计，并能把每个大 mux、memory、balance register 追溯到 source location。否则只能对最终 Verilog 猜原因。

### 过渡

下面用仓库最小的 Counter，把每一层具体串起来。

---

## 第 6 页：Counter 源码——一个变量不等于一个寄存器

### 投影片

```python
enable = cas(domain, m.input("enable", width=1), cycle=0)
count = domain.signal(width=8, reset_value=0, name="count")
m.output("count", wire_of(count))

domain.next()
count.assign(count + 1, when=enable)
```

### 讲稿

这是仓库 `designs/examples/counter/counter.py` 的核心。直觉上，我们会认为这里只声明了一个 8-bit `count` state，next value 是 enable 时加一，否则保持。

但 `domain.next()` 改变了逻辑 occurrence。后面的表达式在 cycle 1 使用前面 cycle 0 的 `count`，cycle-aware frontend 会调用 `delay_to()` 自动补寄存器进行对齐。

当前 raw MLIR 实际出现两个 8-bit `pyc.reg`：一个是 count 自身，另一个是 `_v5_bal_1`。已提交的 `compile_stats.json` 也记录 2 个 reg、16 bits，而不是 1 个 reg、8 bits。

这个例子很适合说明：Python 里的变量数量、MLIR state 数量和 Verilog 文件中的 wire 数量是三件不同的事。真正的资源分析要从 IR 中数 state bits，并理解每个 state 的周期语义。

### 展示代码

- 设计源码：[counter.py:13](../designs/examples/counter/counter.py#L13)
- 自动 delay：[v5.py:308](../compiler/frontend/pycircuit/v5.py#L308)
- 当前统计：[counter compile_stats.json](../designs/examples/counter/build/verilog/compile_stats.json)

### 现场命令

```bash
PYTHONPATH=compiler/frontend python3 designs/examples/counter/counter.py
```

可以搜索 `pyc.reg`，现场指出 `count` 和 `_v5_bal_1` 两级 state。

### 我的判断

这个 balance register 是否多余，不能只从面积判断。必须先确认 cycle contract 和 testbench observation point。如果删除后 latency 或 reset 后可观察行为改变，就不是合法优化。正确做法是在 MLIR 层做 provenance、sharing 和 reset-intent 分析，而不是 Emitter 看到 `_v5_bal` 名字就删。

### 过渡

接下来观察同一结构进入 Verilog Emitter 后长什么样。

---

## 第 7 页：Emitter 做了什么——从 op 到 Verilog 结构

### 投影片

```text
func.func   → module
pyc.add     → assign y = a + b
pyc.mux     → assign y = sel ? a : b
pyc.instance→ module instance
pyc.reg     → pyc_reg primitive instance
```

### 讲稿

Verilog Emitter 的主要入口是 `emitFunc`。每个 `func.func` 变成一个 Verilog module。组合 op 通常输出 continuous assign；层次化的 `pyc.instance` 输出普通 module instance；stateful op 则实例化 runtime primitive。

比如 `pyc.add` 直接写成括号包围的 `lhs + rhs`；`pyc.mux` 写成三目运算符。除法和余数更值得注意：Emitter 显式加入除数为零的比较和 mux，返回零。这是 IR 语义的一部分，不能为省逻辑只在 Verilog backend 删除。

Emitter 还会为每个 op result 声明内部 wire，并根据 `pyc.name` 加注释。这让 RTL 便于追踪，但文件会比手写 RTL 冗长。大部分中间 alias 会被 Yosys `opt` 清掉。

### 展示代码

- module 入口：[VerilogEmitter.cpp:888](../compiler/mlir/lib/Emit/VerilogEmitter.cpp#L888)
- scalar op 输出：[VerilogEmitter.cpp:321](../compiler/mlir/lib/Emit/VerilogEmitter.cpp#L321)
- 除零逻辑：[VerilogEmitter.cpp:346](../compiler/mlir/lib/Emit/VerilogEmitter.cpp#L346)
- 内部 net 声明：[VerilogEmitter.cpp:944](../compiler/mlir/lib/Emit/VerilogEmitter.cpp#L944)

### 讲稿中的一句重点

“Emitter 决定的是表达形式；多数情况下，硬件有几个寄存器、多少路选择、memory 是什么端口，已经由 IR 决定。”

### 过渡

我们看 Counter 的实际生成文件，会看到这种机械 lowering 的结果。

---

## 第 8 页：Counter 生成 Verilog——文本很大，关键资源很少

### 投影片

```verilog
assign pyc_add_11 = (_v5_bal_1 + pyc_comb_6);
assign pyc_mux_12 = (enable ? pyc_add_11 : pyc_comb_8);

pyc_reg #(.WIDTH(8)) pyc_reg_10_inst (...);
pyc_reg #(.WIDTH(8)) pyc_reg_7_inst (...);
```

### 讲稿

当前 `counter.v` 有 66 行。一个手写 counter 可能十几行，所以视觉上会觉得生成器非常臃肿。但资源分析不能按行数。

这里真正值得圈出来的是两个 8-bit `pyc_reg`，一个 8-bit add 和一个 enable mux。`pyc_constant_*`、`pyc_comb_*`、`count_2` 等很多名字主要是 SSA/op 的可追踪映射，综合器会清理其中大量纯连线。

所以我会把“生成 Verilog 很大”拆成两个问题：

1. 文本/编译规模是否过大——影响可读性、parser 时间、debug；
2. post-synthesis cell 是否过大——影响真实资源。

这两个问题可能相关，但不能用第一个替代第二个。

### 展示代码

- 当前生成 RTL：[counter.v](../designs/examples/counter/build/verilog/counter.v)
- module ports、wire、组合区、时序区分别在第 4、11、29、46 行附近。

### 我的判断

减少无用 alias 是有价值的工程优化，但优先级低于减少不必要的 state bits、大 mux 和 memory 展开。必须用 Yosys post-opt cell 证明 alias 清理有面积收益，否则它只是生成文件瘦身。

### 过渡

两个 `pyc_reg` 最终不是由 Counter module 自己写 `always`，而是进入公共 primitive。

---

## 第 9 页：Sequential primitives——reset/enable 会影响资源

### 投影片

```verilog
always @(posedge clk) begin
  if (rst)
    q <= init;
  else if (en)
    q <= d;
end
```

### 讲稿

`pyc.reg` 在 Emitter 中被实例化为 `pyc_reg`，其实现位于 `runtime/verilog/pyc_reg.v`。每个 register 都有同步 reset、enable、data 和运行时 init 输入。

这保证了明确且统一的 reset 行为，但也带来资源影响。对于自动插入的数据 pipeline register，如果每一级都必须 reset，ASIC 可能需要更大的带 reset/enable flop 或额外 mux；FPGA 中也可能妨碍 SRL、BRAM 或 DSP 周边寄存器吸收，同时增加 reset fanout。

这里最危险的“优化”是直接在 Verilog primitive 中去掉 reset。这样可能让 Verilog 省资源，但 C++ simulator 仍然在 reset 时清零，两个 backend 就漂移了。

### 展示代码

- register primitive：[pyc_reg.v](../runtime/verilog/pyc_reg.v)
- Emitter 实例化位置：[VerilogEmitter.cpp:1180](../compiler/mlir/lib/Emit/VerilogEmitter.cpp#L1180)
- reset/init 合同：[Decision 0115](../docs/rfcs/pyc4.0-decisions.md#decision-0115-resetinitialization-semantics-are-explicit-and-identical-across-backends)

### 我的判断

长期方案应在 dialect 中区分 architectural reset-required state 与 reset 后由 valid 屏蔽、初值不可观察的数据 pipeline state。然后由 verifier、C++ runtime 和 Verilog primitive 共同实现。它是语义设计问题，不是一个 backend peephole。

### 过渡

到这里 Verilog 已经生成。下面进入最容易混淆的 Verilator 和 Yosys。

---

## 第 10 页：Verilator 与 Yosys——一个运行行为，一个改变结构

### 投影片

| 工具 | 本项目作用 | 主要输出 |
|---|---|---|
| Verilator | RTL 仿真 | pass/fail、波形、运行 binary |
| Yosys | 综合/结构分析 | cell/memory/netlist/resource stats |
| C++ backend | 同 IR 的另一种仿真 | pass/fail、trace、simulation stats |

### 讲稿

为了术语准确，我后面不再说“Yosys 仿真”，而说“Yosys 综合”或“Yosys 结构分析”。

Verilator 会把生成的 Verilog 和 SystemVerilog testbench 编译成 native executable。testbench 产生 clock/reset，drive 输入，等待 posedge，再检查输出。项目 CLI 使用 `verilator --binary --timing --trace`。

Yosys 不执行这个 testbench。它读取可综合 RTL，选择 top，lower process，优化组合逻辑，识别或映射 memory，最后输出 generic 或 technology-mapped netlist 和统计。

C++ backend 则绕过 Verilog，从同一 MLIR 生成 C++ model。它使用 `eval_comb_pass`、`tick_compute`、`tick_commit` 等 phase。C++ 和 Verilator 都通过，是 cross-backend equivalence 的证据；Yosys cell 数下降，则是结构优化证据。

### 展示代码

- Verilator 调用：[cli.py:2557](../compiler/frontend/pycircuit/cli.py#L2557)
- TB drive/posedge/post-expect：[cli.py:1731](../compiler/frontend/pycircuit/cli.py#L1731)
- C++ 模型相位：[架构文档第 7 节](../docs/v6_PyCircuit_Software_Architecture.md#7-c-发射器与仿真模型)

### 过渡

那当前项目为 Yosys 生成了怎样的脚本？它目前其实只是一条 sanity flow。

---

## 第 11 页：当前 Yosys 脚本——能综合，不等于能做公平 PPA

### 投影片

```tcl
read_verilog -sv pyc_primitives.v
read_verilog -sv <modules>.v
hierarchy -top <top>
proc; opt; memory; opt
synth -top <top>
```

缺少：

```text
stat -json / check -assert / target family / Liberty
memory macro policy / constraints / version+script hash
```

### 讲稿

`pycc --out-dir --emit=verilog` 会自动写 `yosys_synth.ys`。源码注释明确称它为 sanity synth。它读 primitive 和每个 module，设置 top，跑 `proc; opt; memory; opt`，然后 generic `synth`。

它适合回答“Yosys 能否读懂并综合这份 RTL”。但是它没有输出 JSON stats，没有指定 FPGA family 或 ASIC Liberty，没有 clock constraint，也没有统一 memory macro 策略。

所以只跑这个脚本后看一段 console `stat`，不能发表“pyCircuit 比 Chisel 多百分之多少资源”的结论。尤其 memory 一方保留 SRAM/BRAM、另一方被展开成 FF+mux 时，结果会相差数量级。

### 展示代码

- 脚本生成：[pycc.cpp:2487](../compiler/mlir/tools/pycc.cpp#L2487)
- Counter 脚本：[counter/yosys_synth.ys](../designs/examples/counter/build/verilog/yosys_synth.ys)
- 建议的 cut-point 流程：[02_yosys_verilator_flow](02_yosys_verilator_flow.md#5-建议的-yosys-cut-point-流程)

### 我的判断

第一项基础设施任务应当是独立的、版本固定的 Yosys benchmark runner，而不是直接把自动 stub 扩成一个同时服务所有 target 的巨大脚本。sanity、generic comparison 和 target PPA 应分层保存。

### 过渡

为什么要分 cut point？因为 Yosys 在每个阶段回答的问题不同。

---

## 第 12 页：Yosys cut points——资源差异第一次出现在哪里

### 投影片

```text
A. raw MLIR
B. optimized MLIR
C. Yosys post-proc
D. post-memory-collect
E. generic synth
F. target mapping
G. place & route / STA
```

### 讲稿

资源分析最有效的方法不是只比较最终总数，而是找差异第一次出现的阶段。

如果 raw MLIR 已经多了几千个 register，那么问题在 frontend、周期推导或设计结构。raw 正常但 optimized MLIR 变大，要检查 lowering pass。MLIR 正常但 post-proc 变大，才重点看 Emitter 或 primitive pattern。memory collect 之后 `$mem` 消失并变成大量 `$dff/$mux`，则是 memory 语义或 inference 问题。generic 结构接近、target mapping 才拉开，才主要是 ABC/techmap/constraint 问题。

建议每次保存：

```tcl
proc; opt_clean
tee -o 10_proc_stat.json stat -json -top TOP

memory_dff; memory_collect; opt_clean
tee -o 20_memory_stat.json stat -json -top TOP

synth -flatten -top TOP
tee -o 30_generic_stat.json stat -json -top TOP
write_json 30_generic_netlist.json
```

然后 target flow 单独跑 FPGA family 或 ASIC Liberty。

### 我的判断

“第一次出现差异的层”比“最终多了多少 LUT”更能指导工程修改，因为它把责任边界定位到 frontend、MLIR pass、Emitter、primitive 或 mapper。

### 过渡

这里还要特别说明，项目现有的 `compile_stats.json` 不是 Yosys stats。

---

## 第 13 页：`compile_stats.json` 的陷阱

### 投影片

```text
当前 compile_stats：
  ✓ MLIR function definitions 中的 reg/memory inventory
  ✗ top reachable instance-expanded area
  ✗ Yosys cell/LUT/FF/BRAM report
```

### 讲稿

`CollectCompileStatsPass` 对每个 `func.func` 遍历 `pyc.reg`、`byte_mem`、`sync_mem` 和 `sync_mem_dp`，统计 count 和 bits。随后 `pycc` 对 module 中每个函数定义求和一次。

它不从 top 展开实例，不乘 instance multiplicity，也可能把不可达的函数定义算进去；同时没有完整统计 FIFO、CDC 的内部 state，也没有组合 cell。

一个很直观的证据是已提交的 XiangShan `xs_core` 产物。`compile_stats` 报 5341 个 reg、65073 bits 和 24 个 memory，但同目录真正的 `xs_core.v` 只有 13 个直接 `pyc_reg`，而且没有实例化 frontend/backend/memblock。Yosys `hierarchy -top xs_core` 会删除不可达 module，两套数字根本不是同一种统计对象。

### 展示代码

- per-function 收集：[CollectCompileStatsPass.cpp:52](../compiler/mlir/lib/Transforms/CollectCompileStatsPass.cpp#L52)
- module definitions 求和：[pycc.cpp:1944](../compiler/mlir/tools/pycc.cpp#L1944)
- 例子：[xs_core compile_stats](../designs/XiangShan-pyc/build/xs_core/verilog/compile_stats.json)

### 我的判断

建议把现有文件明确命名为 `ir_definition_inventory`，另建 `reachable_instance_stats`、`yosys_generic_stats` 和 `target_resource_stats`。不改名也至少要在 schema 中加入 scope 字段，阻止误用。

### 过渡

统一度量以后，下面看当前最有把握的几个资源根因。

---

## 第 14 页：高风险根因一——Register File 被展开成 FF 和大 mux

### 投影片

```python
regs = [state(64) for entry in range(224)]

for read_port in range(14):
    for entry in range(224):
        data = mux(addr == entry, regs[entry], data)

for write_port in range(8):
    for entry in range(224):
        regs[entry].assign(wdata, when=wen & (waddr == entry))
```

### 讲稿

当前 XiangShan-pyc register file 默认是 224 个 64-bit state，14 个组合读口和 8 个写口。源码直接把每个 entry 声明为带 reset 的 `domain.signal`；每个读口遍历全部 224 个 entry，生成 equality 和 mux chain；每个写口也对每个 entry 做 decode。

仅 state 下限就是 224 乘 64，也就是 14336 个带 reset bits，还不包括 14 组全表读 mux 和 8 乘 224 个写地址比较。

如果 Chisel 或手写对照使用 banked SRAM、复制读 bank、多泵 memory 或 foundry register-file macro，那么它们并不是同一种物理实现。差距不能归因于“Python 生成 Verilog不好”，而应归因于微结构和 memory lowering 不一致。

### 展示代码

- XiangShan-pyc RF 参数和结构：[regfile.py:1](../designs/XiangShan-pyc/backend/regfile/regfile.py#L1)
- 通用 RF 实现：[lib/regfile.py:65](../compiler/frontend/pycircuit/lib/regfile.py#L65)

### 我的判断

不能让 Emitter 把任意 FF+mux 猜成 memory。正确方向是显式 multiport-regfile/memory op，在 dialect 中定义端口数、latency、冲突和 RDW，再由 target-aware pass 选择 banking、replication 或 macro。

### 过渡

第二个高风险来自 cycle-aware frontend 自动插入的流水寄存器。

---

## 第 15 页：高风险根因二——Cycle balance register

### 投影片

```python
def delay_to(w, from_cycle, to_cycle, width):
    for _ in range(to_cycle - from_cycle):
        r = m.out(..., width=width, init=0)
        r.set(w)
        w = r.q
    return w
```

```text
Bypass 小参数定位实验：
bypass_unit.py     0 reg / 0 bits
bypass_unit_v5.py 352 reg / 1456 bits
```

### 讲稿

`CycleAwareSignal` 运算时会把操作数对齐到最大逻辑 cycle。当前 `delay_to()` 对每一拍直接创建一个新的 `_v5_bal_N` register，而且 `init=0`，因此每一级都进入带同步 reset 的 `pyc_reg`。

我们对当前 Bypass 源码做过 frontend-only 小参数定位：`lanes=4, data_width=32, ptag_count=64`。普通版本 raw MLIR 没有 `pyc.reg`；cycle-aware 版本有 352 个 register、1456 bits。

这个实验只能证明差异在 Emitter 之前出现，不能证明 352 个全部冗余，因为两个版本的 pipeline semantics 不同。对 exact source/cycle/width 做简单对象 cache 也没有降低该 case 的数量，说明问题可能涉及 vector extraction、结构等价值和 delay-chain prefix sharing。

### 展示代码

- 自动平衡实现：[v5.py:308](../compiler/frontend/pycircuit/v5.py#L308)
- Bypass 结构：[bypass_unit_v5.py](../designs/BypassUnit/bypass_unit_v5.py)
- 已提交旧产物：[Bypass compile_stats](../designs/BypassUnit/build/verilog/compile_stats.json)

### 我的判断

建议先做 balance provenance analysis：每个 register 记录 canonical source、from/to cycle、domain、width、reset intent 和 fanout。然后在 MLIR 层共享相同 delay chain 和前缀，做 vector-wide canonicalization。这样收益才可解释，也能受 G2 equivalence 保护。

### 过渡

除了 register 数，组合网络的形状也会让 Yosys/ABC 得出非常不同的映射。

---

## 第 16 页：高风险根因三——Priority mux 与 one-hot 合同

### 投影片

```text
true priority：多 hit 时 index-0-wins
proven one-hot：最多一个 hit，可以使用不同拓扑

语义不同，不能无条件替换
```

### 讲稿

`Vec.priority_mux` 默认提供真实的 index-0-wins priority 语义，因此从反方向构造线性 mux chain。源码注释记录：如果 selector 实际是 one-hot，这个 reverse chain 在某次 ABC 实验中比 forward chain 映射得明显更差；128:1、64-bit read mux 曾观察到大约多 500 LUT。

同时，如果 selector 是 vector op 的结果，再逐 lane `self[i]`，会产生 `v_get` extraction。这些残留 wire 虽然逻辑等价，也可能扰动 ABC 的 cut heuristic。

但不能简单把默认 `assume_onehot` 改成 true。如果同一拍有两个 hit，true-priority 和 one-hot lowering 的结果不同。优化前必须有 one-hot/onehot0 合同，并证明或动态检查它。

### 展示代码

- 实现和已有注释：[hw.py:3427](../compiler/frontend/pycircuit/hw.py#L3427)

### 我的判断

应该把 one-hot 信息变成 MLIR 属性或专用 op：静态分析能证明时自动添加；作者声明时由 verifier/assertion/formal gate 保护。之后才能安全比较 forward chain、masked OR 和 tree。true-priority 则保留原语义，必要时研究分层 priority encoder。

### 过渡

讲到这里，也要排除几个“看起来很大但通常不是主要资源”的因素。

---

## 第 17 页：哪些通常不是面积主因

### 投影片

```text
通常不是直接面积：
  大量 wire / alias
  vector pack/unpack slice assigns
  pyc.assert（SYNTHESIS 下排除）
  module 文件数量本身

必须用 post-opt netlist 证明
```

### 讲稿

Emitter 会为 vector module port 做 packed bus，在内部用 unpacked array，再生成逐 lane pack/unpack assign。层次实例之间也会有 `__flat` bridge。它们让 Verilog 变长，但通常只是 bit-level 连接。

每个 op result 都声明 wire，也会增加文本。`pyc.assert` 则包在 `` `ifndef SYNTHESIS `` 中，综合时不进入面积。

这些结构不是永远没有间接代价。例如某种 bridge 可能阻碍 memory 或 mux pattern recognition，冗长 RTL 也会增加 parser 时间。但这种结论必须通过 post-`proc; opt_clean` netlist A/B 证明，不能只靠肉眼。

### 展示代码

- vector port flatten：[VerilogEmitter.cpp:65](../compiler/mlir/lib/Emit/VerilogEmitter.cpp#L65)
- pack/unpack：[VerilogEmitter.cpp:134](../compiler/mlir/lib/Emit/VerilogEmitter.cpp#L134)
- simulation-only assert：[VerilogEmitter.cpp:1056](../compiler/mlir/lib/Emit/VerilogEmitter.cpp#L1056)

### 我的判断

优化优先级应是 state/memory/mux 拓扑高于 RTL cosmetic cleanup。文本瘦身可以做，但要单独报告编译时间和文件大小收益，不与 LUT/area 混在一起。

### 过渡

资源优化之外，我还发现一个会影响正确性和后续优化安全性的门禁缺口。

---

## 第 18 页：Correctness gap——multiple driver 没有在 MLIR 被拒绝

### 投影片

```text
AssignOp::verify：只检查 dst 来自 pyc.wire

Emitter topo sort：发现 multiple drivers → false
Caller：失败后改成 lexical sort，仍继续发射

应当：MLIR verifier 先拒绝或显式 resolved-net
```

### 讲稿

当前 `AssignOp::verify()` 只检查 destination 是否由 `pyc.wire` 定义，没有检查 driver 数量。

Emitter 的组合拓扑排序会统计同一 wire 的 assign 数。如果发现多个 continuous driver，它返回 false。但调用者没有报错，而是退回字典序继续发射这些组合 op。

这和 Decision 0130 的 net/var split、single-driver var、explicit resolved-net 合同之间有缺口。它首先是 correctness 问题，也让 wire elimination 和表达式共享难以安全进行。

### 展示代码

- 当前 verifier：[PYCOps.cpp:909](../compiler/mlir/lib/Dialect/PYC/PYCOps.cpp#L909)
- Emitter 检测：[VerilogEmitter.cpp:819](../compiler/mlir/lib/Emit/VerilogEmitter.cpp#L819)
- 失败后的 fallback：[VerilogEmitter.cpp:1048](../compiler/mlir/lib/Emit/VerilogEmitter.cpp#L1048)

### 我的判断

正确顺序是先加 instance-aware driver legality verifier；普通 var 必须单 driver；确需多 driver 的情况使用显式 resolved-net op；C++ 和 Verilog backend 同时消费已验证语义；Emitter 对非法 IR 直接失败。

### 过渡

最后我把前面的观察收敛成一个可执行路线。

---

## 第 19 页：建议路线——先度量和 gate，再做结构优化

### 投影片

```text
P0  公平 benchmark + Yosys JSON + reachable stats
P1  single-driver / reset-intent / one-hot / memory gates
P2  cycle sharing / vector canonicalization / regfile lowering
P3  Emitter 可读性与文本规模
```

### 讲稿

第一阶段 P0 是建立公平 benchmark。每个 case 固定 top、parameters、latency、reset、memory RDW、blackbox 和 synthesis script。生成 top-reachable instance stats，并保存 Yosys proc、memory、generic 和 target JSON。

第二阶段 P1 是语义门禁。补 single-driver verifier；定义 reset intent；让 one-hot 成为可验证合同；增加 memory inference litmus，确认期望的 `$mem` 没有意外展开。

第三阶段 P2 才是主要结构优化：cycle-balance delay sharing、vector-wide canonicalization、one-hot mux lowering，以及 explicit multiport register-file/memory op 和 target macro mapping。

第四阶段 P3 再做 Emitter 输出瘦身、sidecar source map 和更好的 module summary。

每个优化 PR 都应该报告：decision IDs、G1/G2 gate、tool version、benchmark hash，以及 raw IR、optimized IR、post-proc、post-memory、generic、target 六层 before/after。

### 展示代码

- 完整路线：[04_optimization_roadmap](04_optimization_roadmap.md)

### 我的判断

我认为第一批最适合上手的任务是 top-reachable stats、single-driver verifier 和 balance provenance。它们风险相对可控，却能显著提升后续所有资源优化的可信度。

### 过渡

最后用四句话结束。

---

## 第 20 页：总结

### 投影片

```text
1. MLIR 是语义中心，Emitter 是 lowering 边界
2. Verilator 验证行为，Yosys 分析/综合结构
3. 资源先看 state、memory、mux，再看 RTL 文本
4. 公平基线 + 分层统计 + G1/G2 是优化前提
```

### 讲稿

第一，pyCircuit 不是简单模板打印器。Frontend 先构造 PYC MLIR，`pycc` 做合法化和优化，Emitter 再生成 C++ 或 Verilog。语义应该停留在 dialect 和 passes。

第二，Verilator 和 C++ backend 回答逐周期行为是否正确；Yosys 回答 RTL 被识别成什么结构。`compile_stats.json` 只是 IR inventory，不能替代 Yosys 或 target report。

第三，当前最有把握的资源风险是 FF 化的多端口 register file、自动 cycle-balance register、全量同步 reset，以及 priority/one-hot mux 拓扑。大量 wire 和 pack/unpack 更可能是文本问题，必须由综合网表证明其实际成本。

第四，没有同语义 top、同参数、同 memory policy、同综合脚本，就没有公平的 pyCircuit/Chisel/手写比较。优化必须先有 G1 legality 和 G2 cross-backend equivalence。

我的讲述到这里。后面可以围绕三个问题讨论：我们选哪个等价 benchmark；先补哪一个 stats/gate；register file 和 cycle balance 哪个更适合作为第一项结构优化。

---

# 现场演示预案

## 演示 A：Counter 的 source → raw MLIR → Verilog

预计 4 分钟。

```bash
sed -n '13,22p' designs/examples/counter/counter.py

PYTHONPATH=compiler/frontend \
python3 designs/examples/counter/counter.py

sed -n '1,90p' designs/examples/counter/build/verilog/counter.v

sed -n '1,30p' designs/examples/counter/build/verilog/compile_stats.json
```

讲解顺序：

1. Python 只有一个名为 `count` 的 architectural state；
2. raw MLIR 搜索 `pyc.reg`，看到 count 和 balance 两个；
3. Verilog 搜索 `pyc_reg #`，看到两个 primitive instance；
4. `compile_stats` 显示 2/16；
5. 强调差异在 Emitter 前已经发生。

## 演示 B：Emitter 中追踪 `pyc.add` 和 `pyc.reg`

预计 3 分钟。

```bash
sed -n '321,375p' compiler/mlir/lib/Emit/VerilogEmitter.cpp
sed -n '1180,1195p' compiler/mlir/lib/Emit/VerilogEmitter.cpp
sed -n '1,30p' runtime/verilog/pyc_reg.v
```

讲解顺序：

1. add/mux 是机械式 `assign`；
2. register 是 primitive instance；
3. primitive 里才有 clocked `always`；
4. reset/init 是 dialect/backend 共同合同。

## 演示 C：Yosys 可用时

当前报告编写环境没有 Yosys。现场环境若已固定版本，可在 Counter build 目录运行：

```bash
cd designs/examples/counter/build/verilog
yosys -s yosys_synth.ys
```

注意：这只能展示 sanity synthesis。若要展示资源，应提前准备增加 `tee ... stat -json` 的独立脚本和保存好的结果，避免现场临时用不固定版本做结论。

## 演示失败时的备用材料

- raw MLIR 可用 `counter.py` 直接生成，不依赖 `pycc`；
- committed `counter.v`、`compile_stats.json`、`yosys_synth.ys` 均可静态展示；
- 不要临时安装 Yosys 或更换综合版本后比较资源。

---

# 常见提问与建议回答

## Q1：为什么不直接让 Yosys 优化，编译器还要做这么多 pass？

建议回答：Yosys 擅长通用 RTL 逻辑综合，但它不知道 pyCircuit 的高层 cycle、one-hot 声明、source provenance、Bundle layout 和跨 backend 语义合同。MLIR pass 可以在信息尚未丢失时做更安全、可诊断的优化，同时提前拒绝 comb loop、CDC 和非法类型。两层优化是互补关系。

## Q2：为什么不用 Verilog 行数比较生成质量？

建议回答：行数混合了真实逻辑和命名/连线。1000 行 slice assign 可能是零面积连接，一个 224×64、14 读 8 写 register file 也可能只由几十行 Python 循环生成。应比较 post-opt/generic/target cell，并保留文本规模作为独立工程指标。

## Q3：Counter 多出的 balance register 是 bug 吗？

建议回答：它是当前 cycle-aware 语义下 `domain.next()` 对齐产生的真实结构。是否是 bug，要看该示例期望的 occurrence/latency 和两 backend test。现阶段它能证明资源在 frontend 已经出现，但不能仅凭“手写 counter 只需一个寄存器”就 backend 删除。应先建立 cycle/observation contract 和最小 litmus。

## Q4：`compile_stats` 为什么和 Yosys 不一致？

建议回答：它统计 IR 中每个 function definition 一次，不按 top reachable instance 展开，也不是 technology mapping 结果。Yosys 会根据 top 删除不可达 module，并把 register/memory/logic映射成另一套 cell。二者 scope 不同。

## Q5：Chisel 对照是不是天然更优？

建议回答：不能这样推断。必须确认双方的参数、latency、reset、memory macro、blackbox 和 top connectivity 完全一致。当前 XiangShan-pyc 是按规范重新实现，原 Chisel 只作为参考；同名 module 不能证明微结构等价。

## Q6：为什么不能在 Emitter 中针对 one-hot 直接换一种 mux？

建议回答：因为 Emitter 看不到或不能证明 selector 一定 one-hot。多 hit 时 priority 结果与 one-hot lowering 不同。应先在 IR 中表达并验证 one-hot 合同，再让 canonical lowering 选择结构。

## Q7：资源优化应该先做 Register File 还是 Cycle Balance？

建议回答：如果追求最大单点收益，register file/memory mapping 可能更大；如果追求建立通用优化能力，balance provenance 和 sharing 更适合先做。无论选哪个，P0 的公平 benchmark 和分层 stats 都应先完成。

## Q8：Yosys generic cell 数能否代表 FPGA LUT？

建议回答：不能直接等同。generic `$mux/$add/$dff` 有助于解释结构，但 LUT 数依赖 family、carry chain、ABC mapping、flatten 和约束。必须再跑固定 target synthesis；ASIC 则要固定 Liberty 和 memory macro policy。

---

# 20 分钟精简版

如果时间只有 20 分钟，保留：

1. 第 1 页：四个结论；
2. 第 2 页：术语校准；
3. 第 3 页：总数据流；
4. 第 5 页：`pycc` pipeline；
5. 第 6–9 页合并：Counter 从 Python 到两个 `pyc_reg`；
6. 第 10–13 页合并：Verilator/Yosys/compile_stats 区别；
7. 第 14–16 页：RF、cycle balance、priority mux 三个根因；
8. 第 19–20 页：路线和总结。

删除 elaboration 细节、false-cause 独立页和 multiple-driver 细节，把它们放到问答。

---

# 讲者最后检查清单

- 不把 Yosys 称为本项目的 RTL 仿真器；
- 不把 `compile_stats.json` 称为 Yosys 资源报告；
- 不把 committed build artifact 与当前源码/参数混成同一 baseline；
- 不用 Verilog 行数代替面积；
- Counter 的两个寄存器只作为“定位层级”证据，不直接宣判冗余；
- one-hot 优化前说明多 hit 语义；
- register file 对比前说明 memory macro 和端口合同；
- 明确哪些结论已由源码证明，哪些仍需固定 Yosys/target A/B；
- 所有语义优化回到 dialect/verifier/pass，并同时过 C++/Verilog equivalence。
