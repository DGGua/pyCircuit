# Delay-line 与周期状态优化

本文说明 pyCircuit 周期状态优化的设计、实现、正确性边界和实测效果，供设计评审、
性能分析和后续维护使用。当前实现覆盖：

- 一等 MLIR 状态操作 `pyc.delay_line`；
- Stage 0 状态优化机会分析；
- Stage 1 等价状态合并、结构化寄存器链识别和重复 delay-line 共享；
- Stage 1.5 有界的 canonicalize/CSE 与第二轮状态细化；
- Stage 2 通用整数 state-lane packing；
- 默认性能优先的显式观测身份清理；
- C++/Verilog lowering、统计、probe/trace 适配和跨后端等价门禁。

详细实验记录见
[reports/delay_line/](../reports/delay_line/README.md)，专项复现和验证脚本见
[verification/delay_line_combine/](../verification/delay_line_combine/README.md)。

文档状态：2026-08-19，描述 `cycle-combine` 分支当前实现和默认策略。

## 1. 问题、目标与当前结论

前端为了对齐参与同一运算的信号周期，会生成中间寄存器。例如：

```text
c = a@0 + b@4

a ─▶ reg0 ─▶ reg1 ─▶ reg2 ─▶ reg3 ─┐
b@4 ────────────────────────────────┴─▶ add ─▶ c
```

逐级寄存器的 RTL 时序是正确的，但直接映射到 C++ model 时，每一级都会成为独立
状态对象，并产生独立的 compute/commit 调度。长链、重复状态和大量窄状态 lane 会
显著增加：

- MLIR 和生成 C++/Verilog 文本体积；
- C++ 状态对象数量；
- 每周期状态 primitive 的静态调度次数；
- C++ 编译时间和仿真运行时间。

当前方案不是简单地“识别带名字的 cycle-balance 寄存器”，而是把 provenance 和
合法性分开：

- `pyc.generated = "cycle_balance"` 说明状态来源；
- 状态类型、clock/reset/enable/init、next-state、fanout 和依赖关系决定能否改写；
- `generated` 模式只处理原有 marker，作为兼容回退；
- 默认 `structural` 模式不要求 marker，依据 MLIR 结构证明执行优化；
- 默认性能模式允许丢弃内部 debug/probe/trace/name 的物理状态身份；
- 端口行为、周期数、reset/enable/init 和功能数据流仍必须等价。

当前默认配置为：

```text
--state-delay-opt=structural
--state-pack-width=192
--state-opt-preserve-observability=false
```

在 `xs_core` workload 上，相对完全关闭状态/delay 优化，默认配置将：

- compile-stats `reg_count` 从 29,364 降到 7,171，减少 75.58%；
- logical state bits 从 147,364 降到 92,171，减少约 37.45%；
- C++ header 从 38,489,749 B 降到 20,732,146 B，缩小 46.15%；
- 同机 `pycc --emit=cpp` 从 22.40 s 降到 18.06 s，改善 19.38%。

这些数据说明的是 C++ model 结构和生成性能，不等价于 RTL 综合面积同比下降。

## 2. 核心语义：`pyc.delay_line`

`pyc.delay_line` 定义在
[`PYCOps.td`](../compiler/mlir/include/pyc/Dialect/PYC/PYCOps.td)，输入为
`clk, rst, en, next, init`，输出最终一级 `q`：

```mlir
%q = pyc.delay_line %clk, %rst, %en, %input, %init
     {depth = 4 : i64} : i8
```

`depth=N` 等价于 N 个共享 clock/reset/enable/init 的串行 `pyc.reg`：

```text
q0 <= next
q1 <= q0
...
q[N-1] <= q[N-2]
q = q[N-1]
```

语义约束为：

1. 延迟按 enabled positive edge 计数，`en=0` 时整条历史保持；
2. reset 优先于 enable，reset edge 将全部历史位置设置为 `init`；
3. C++ 的 `tick_compute()` 只计算下一状态，`tick_commit()` 后状态才可见；
4. `next/init/q` 类型一致；
5. `depth` 必须存在且大于 1；
6. scalar 和 vector 数据都可使用。

Verifier 位于
[`PYCOps.cpp`](../compiler/mlir/lib/Dialect/PYC/PYCOps.cpp)。depth=1 继续使用
`pyc.reg`，避免两种 op 表达同一种基础状态。

### 2.1 Delay-line 不是 FIFO

Delay-line 没有 ready/valid、full/empty、backpressure 或独立入队/出队。它是在同一
enable 下推进的固定深度历史，不能替代 `FIFO(max_depth)`。

### 2.2 一个 delay-line op 不等于一个寄存器

`pyc.delay_line depth=N` 是紧凑 IR 表示，仍承载 N 级逻辑状态。width=W 时：

```text
logical register count = N
logical state bits     = N × W
```

单链合并减少的是状态对象、IR、生成文本和 C++ 调度。只有等价状态合并、dead-state
删除或重复历史共享才会减少 logical state bits。

## 3. 设计原则

### 3.1 语义在 Dialect 和 MLIR Pass 中

所有状态改写都在 MLIR 中完成。C++ 和 Verilog backend 只 lowering 已证明合法的
`pyc.reg`、`pyc.delay_line`、concat/extract 等结构，不在 backend 根据名字或
私有属性补语义。这符合仓库的 gate-first 和 no backend-only semantic fix 约束。

### 3.2 Provenance 不是正确性证明

早期实现把 `pyc.generated = "cycle_balance"` 当作候选资格，安全但覆盖有限。
当前设计中 marker 仍用于来源统计和兼容模式，但 structural 模式的正确性来自完整
状态转移条件，而不是来源名字。

### 3.3 区分四类指标

讨论收益时必须区分：

| 指标 | 含义 |
|---|---|
| `reg_bits` / logical state bits | 设计状态容量；delay depth 和 width 都要计入 |
| `reg_count` | 每个普通/packed reg 计 1，delay-line 按 depth 计；不是原始 MLIR op 数或综合 FF 数 |
| state op / primitive count | backend 实际需要声明和调度的 reg/delay-line 对象数量 |
| 生成文本/编译耗时 | C++/Verilog emitter 和主机编译器成本 |
| RTL 综合资源 | Yosys/目标综合器给出的 FF、LUT、memory 等结果 |

State packing 通常降低 primitive count，但不降低 logical state bits。C++ ring
delay-line 能大幅减少每周期调度，但 Verilog 仍要保留真实 N 级时序。

### 3.4 有界优化优先于无界 fixed point

Stage 1.5 固定运行一次 canonicalize+CSE 和第二轮状态优化。这样能捕获主要级联收益，
同时保证编译时间、统计口径和 IR 结果可预测，不引入数据相关的无界迭代。

## 4. 默认流水线和策略开关

状态优化在 wire/dead-state 清理之后、clock/comb/logic-depth legality gates 之前运行：

```text
eliminate-wires
eliminate-dead-state
        │
        ▼
Stage 0: analyze-state-optimization
        │
        ├─ structural performance mode:
        │    strip-state-observability
        │    eliminate-dead-state
        ▼
Stage 1: merge equivalent state / form delay lines / share delay lines
        │
        ▼
Stage 1.5: canonicalize + CSE + bounded second state round
        │
        ▼
Stage 2: pack compatible reg/delay-line lanes
        │
        ▼
comb/clock/logic-depth gates → stats → C++ or Verilog emitter
```

`pycc` 的主要开关：

```text
--state-delay-opt=off
    只运行 Stage 0 分析，不执行状态/delay 改写

--state-delay-opt=generated
    只处理 pyc.generated="cycle_balance"，保留原有观测边界

--state-delay-opt=structural
    使用 provenance-independent 状态证明；当前默认

--state-pack-width=192
    Stage 2 单个 packed storage 的默认最大宽度

--state-pack-width=0
    禁用 Stage 2，便于隔离 Stage 1/1.5 收益

--state-opt-preserve-observability=true
    保留内部命名/debug/probe/trace 状态身份；性能较低
```

legacy `--combine-delay-chains=false` 强制 `off`。未显式设置
`--state-delay-opt` 时，显式 legacy `--combine-delay-chains=true` 选择
`generated` 兼容行为；新自动化应使用分级参数直接表达策略。

编译统计 JSON 会记录：

```json
{
  "state_opt_policy": "structural",
  "state_opt_preserve_observability": false,
  "state_opt_pack_width": 192
}
```

自动化可直接审计实际策略，不必从命令行默认值反推。

## 5. Stage 0：机会分析和观测边界

`pyc-analyze-state-optimization` 只分析，不改 IR。它在 off、generated 和
structural 三种策略下都运行，记录：

- 看到的 reg 数和 cycle-balance reg 数；
- 被显式观测身份 pin 住的状态数；
- 完全等价状态候选；
- generated 和 structural 串行链候选；
- 只有 structural 模式才能利用的链。

Stage 0 使用
[`StateObservabilityAnalysis`](../compiler/mlir/lib/Transforms/StateOptimization.cpp)
识别以下物理状态身份：

- `pyc.debug_keep`；
- `pyc.observable`；
- `pyc.probe*`、`pyc.trace*`；
- 稳定的非 cycle-balance `pyc.name`；
- 状态 q 之后携带上述属性的 alias 链。

Stage 0 在清理显式观测身份之前运行，所以即使默认性能模式随后放弃这些身份，统计
仍能报告原始设计中有多少状态被标记为可观测。

`state_opt_merge_candidates` 也采用这一保守观测边界。默认性能模式清理 identity 后，
Stage 1 的实际 `state_opt_regs_merged` 可能略高于 Stage 0 候选数；这不是统计错误，而是
两个字段分别回答“保留原始观测时有多少机会”和“当前策略实际改写了多少状态”。

## 6. 默认性能模式：显式观测身份清理

默认 structural 模式运行
[`StripStateObservabilityPass.cpp`](../compiler/mlir/lib/Transforms/StripStateObservabilityPass.cpp)。
该 pass：

1. 从 reg、delay-line 及其状态 alias 删除 `pyc.name`、`pyc.debug_keep`、
   `pyc.observable`、`pyc.probe*` 和 `pyc.trace*`；
2. 删除已经没有 SSA 使用的状态 alias；
3. 再运行 dead-state，删除仅因观测标记而存活、没有功能用途的状态。

需要准确理解“放弃显式观测语义”的范围：

- 允许内部状态失去原物理对象、名字和独立 current/next/pending probe 身份；
- 允许仅用于 debug/probe 的状态和 alias 被删除；
- 不允许改变 function result、instance 端口或其他功能消费者看到的值；
- 不放宽 clock/reset/enable/init、类型、fanout 和状态依赖证明；
- 不允许少一个周期或多一个周期。

例如，只有 `debug_keep` 但没有功能使用的寄存器可被删除；直接返回到模块输出的
寄存器不能因为去掉名字而被删除。

需要调试物理状态时使用：

```bash
pycc input.mlir --state-opt-preserve-observability=true ...
```

该模式下 debug/probe/trace/observable 和具名 alias 继续作为硬边界。只有单纯带
`pyc.name` 的逻辑状态可在安全时通过具名 slice alias 参与 packing；C++
ProbeRegistry 使用 `addRegSlice<W, StorageW>` 将逻辑 q/pending/qNext 映射到
packed storage。

`generated` 兼容模式始终保留观测边界，不受默认 performance policy 影响。

## 7. Stage 1：等价状态、结构化链和共享

Stage 1 由
[`CombineDelayChainsPass.cpp`](../compiler/mlir/lib/Transforms/CombineDelayChainsPass.cpp)
和
[`StateOptimization.cpp`](../compiler/mlir/lib/Transforms/StateOptimization.cpp)
实现，依次执行三个子步骤。

### 7.1 等价状态合并

两个 `pyc.reg` 只有完整状态转移相同才可合并：

```text
(q type, clock, reset, enable, next, init)
```

比较规则为：

1. 剥离透明 `pyc.alias`；
2. 接受相同 SSA value；
3. 对常量接受类型相同且 `value` 属性相同；
4. 不在状态 pass 内证明任意动态组合表达式等价。

clock/reset 同样按透明 alias 归一化，因此前端写出的不同连接 alias 不会制造假
差异。动态表达式的等价由前面的 canonicalize/CSE 建立共同 SSA value，再交给状态
pass 使用。

若两个状态满足完整等价条件，就保留一个 survivor，将另一个 q 的使用替换为
survivor q，并记录：

```text
pyc.optimized_by = "merge_equivalent_state"
pyc.state_merged_count
```

这类优化会真实减少 logical state bits，因为两个状态机从初值开始在每个周期都相同。

### 7.2 Provenance-independent 串行链识别

structural 模式逆序选择 tail，并沿 consumer 的 `next` 向前寻找 predecessor。
每条链边必须满足：

1. 中间只经过透明 `pyc.alias`；
2. predecessor 和 tail 的 q 类型一致；
3. clock、reset、enable、init 语义等价；
4. predecessor q 及路径上的 alias 至少有当前链这一条 state-to-state 使用；structural
   性能模式允许其它只读 consumer，后续会改写为固定深度 tap；
5. 链长度至少为 2；
6. predecessor 路径只能由 `pyc.reg` 和透明 `pyc.alias` 构成，因此不会穿过
   module/instance、memory、FIFO 或 CDC 语义边界。

反向搜索停止时得到最大安全链：

```text
depth      = chain length
input      = head.next
controls   = head clock/reset/enable/init
replacement= tail.q → delay_line.q
```

`generated` 模式还额外要求所有 reg 和透明 alias 都带
`pyc.generated="cycle_balance"`。structural 模式不要求该 marker。

### 7.3 中间只读 fanout 与 delay tap

早期实现要求中间 q one-use，因为 `pyc.delay_line` 只暴露末端 q。当前 structural
性能模式进一步识别只读 fanout：

```mlir
%q0 = pyc.reg ... %input ... : i8
%q1 = pyc.reg ... %q0    ... : i8
return %q0, %q1 : i8, i8
```

```text
%q0 = pyc.reg ... %input ... : i8
%q1 = pyc.reg ... %q0    ... : i8
%q2 = pyc.reg ... %q1    ... : i8
%side = pyc.add %q1, %c : i8, i8 -> i8
return %side, %q2
```

若 `%q1` 的旁路只读，不参与另一条 state 写回依赖，pass 会生成：

```text
%line = pyc.delay_line %input ... {depth = 3}
%tap = pyc.delay_tap %line {depth = 2}
```

`pyc.delay_tap` 是只读视图，不拥有第二份状态。其 verifier 要求 source 必须是
`pyc.delay_line`，tap depth 在 `1..line.depth`，并且类型一致。C++ ring 从同一个
history array 读取 tap；Verilog 从同一组 stages 的只读 history bus 取 slice。这样
旁路值和末端值都保持原来的周期关系，状态存储仍只有一份。

默认性能模式仍不会忽略真实 state dependency、写回 fanout、组合环或跨 block 边界。
显式保留观测模式不启用 fanout tap rewrite，以保留中间状态身份。

### 7.4 完全相同 delay-line 的共享

`pyc.delay_line` 有 MemoryEffects，普通 CSE 不会合并。Pass 显式比较完整键：

```text
(depth, q type, clock, reset, enable, input, init)
```

键完全等价时，后一个 delay q 替换为 survivor q，并删除重复历史。正确性来自状态
转移归纳：相同初始状态和相同的每次状态转移产生相同的每周期 q。

例如同一输入两次延迟两拍：

```text
优化前：a → reg0 → reg1 → c0    共 4×8 = 32 bit
        a → reg2 → reg3 → c1

优化后：a → delay(depth=2) ─┬→ c0  共 2×8 = 16 bit
                             └→ c1
```

这是实际重复状态消除，不是统计折叠。

当前等价状态和 delay sharing 都先按 hash 分桶，再在桶内做完整等价比较。hash 只用于
缩小搜索范围，最终正确性不依赖 hash 相等。

## 8. Stage 1.5：为什么需要第二轮

第一次状态合并可能使原先不同的组合锥获得相同输入：

```text
before:
  q0 != q1 as SSA values
  f(q0) and f(q1) are separate comb nodes

after first state merge:
  q1 uses are replaced by q0
  f(q0) and f(q0) become CSE candidates
```

因此流水线执行：

```text
first merge/form/share
        ↓
canonicalize + CSE
        ↓
second merge/form/share
```

第二轮可识别下游级联的等价状态或新暴露的链。流水线固定两轮，不跑到 fixed point。
统计字段：

- `state_opt_merge_rounds`：structural 模式实际合并轮数；
- `state_opt_cascade_regs_merged`：第二轮新增的等价状态合并数。

Stage 1.5 只共享已经因状态合并而等价的组合锥，不移动组合逻辑穿过寄存器边界。

## 9. Stage 2：通用 state-lane packing

### 9.1 动机

大量彼此独立的 1-bit、8-bit 或其他窄状态即使无法形成串行 delay-line，也会产生大量
C++ 状态对象和 compute/commit 调度。把控制兼容的 lane 放入较宽 storage，可以减少
primitive 数而保持每个 lane 的值和周期语义。

```text
before:
  reg<i4>, reg<i8>, reg<i3>

after:
  concat(next2, next1, next0)
       ↓
  reg<i15>
       ↓
  extract<0:3>, extract<4:11>, extract<12:14>
```

同样的变换适用于 depth 相同的多个 `pyc.delay_line`。

### 9.2 分组合法性

[`PackStateLanesPass.cpp`](../compiler/mlir/lib/Transforms/PackStateLanesPass.cpp)
只在同一 block 内分组。组内必须满足：

- 都是 integer state，且单 lane width 不超过 max width；
- 全部是 reg，或全部是 delay-line，二者不混合；
- delay-line 的 depth 完全相同；
- clock/reset 在剥离透明 alias 后等价；
- enable 为相同 SSA，或类型和值相同的常量；
- 插入 packed state 的位置支配所有 lane consumer；
- 组内不存在会因改写次序失效的状态依赖；
- 总宽度不超过 `--state-pack-width`。

各 lane 的 next/init 不要求相同，它们按确定顺序 concat；packed q 再按原 layout
extract。每个 extract 携带：

```text
pyc.state_pack_lsb
pyc.state_pack_source_width
```

packed state 携带：

```text
pyc.optimized_by = "pack_state_lanes"
pyc.state_pack_lanes
pyc.state_pack_width
```

实现先按 `(state kind, delay depth, normalized clock/reset/enable)` 做 hash lookup，再在
候选桶内执行完整控制等价比较。桶和 lane 均按 block 顺序创建，DenseMap 只用于查找，
不会让 hash iteration 改变生成 IR。随后按源顺序贪心累积 lane；遇到宽度上限、依赖
边界或 dominance 不成立就结束当前 group。这个策略不保证理论最少 group 数，但结果
确定、复杂度可控，也便于对照 pass dump 审阅。

实现会在真正改写每组时重新读取 live state operands，避免前一组删除 alias 后使用
缓存 SSA value。这一约束由 cross-bucket alias dependency 回归覆盖。

### 9.3 Packing 改变什么，不改变什么

Packing：

- 减少 MLIR/backend state primitive 和 C++ 调度次数；
- 通常缩小生成 header；
- 不改变每个逻辑 lane 的 reset、enable、init、next 和 q；
- 不因自身减少 logical state bits；
- 可能使内部物理状态名字变成 packed storage 的 slice。

直接成为模块输出的 packed slice 仍被 probe manifest 分类为 state。在显式观测保留
模式下，具名逻辑 state 可通过 slice alias 和 `addRegSlice` 保持逻辑 probe 映射。

### 9.4 为什么默认宽度是 192

更宽的 storage 会减少 primitive 数，但超宽 `Bits<W>` 的 concat/extract、copy 和
更新成本也会上升。默认值不是“越宽越好”，而是由 C++ 微基准选择。

128 个独立 8-bit state、500,000 cycles、每种宽度 3 次取中位数：

| max width | median ns/cycle | sequential primitives | header bytes |
|---:|---:|---:|---:|
| 0 | 6,493.52 | 128 | 75,153 |
| 128 | 4,888.44 | 8 | 41,413 |
| 192 | 4,362.49 | 6 | 40,979 |
| 256 | 6,051.11 | 4 | 38,052 |

192-bit 相对不打包快 32.8%，相对 128-bit 快 10.8%。256-bit 虽然 primitive 更少，
但超宽运算成本抵消了调度收益，因此默认选择 192。

性能模式还会运行旧 `PackI1RegsPass`，收集通用 dependency-aware packer 留下的
i1 lane。显式观测保留模式跳过旧 i1 packer，因为它不保留观测元数据。

## 10. 后端表示

### 10.1 C++ delay-line：环形历史

优化前每一级都是独立 `pyc_reg`；优化后生成：

```cpp
pyc_delay_line<8, 128> *delay;
delay->tick_compute();
delay->tick_commit();
```

[`pyc_primitives.hpp`](../runtime/cpp/pyc_primitives.hpp) 内部保存：

```cpp
std::array<Wire<Width>, Depth> stages{};
unsigned head = 0;
```

正常 enabled edge 只读一个历史槽、写一个槽并推进 `head`，从 O(depth) 个 primitive
调度降为 O(1)。reset 必须填充全部历史位置，仍为 O(depth)。scalar/vector 分别使用
`pyc_delay_line<Width, Depth>` 和 `pyc_vec_delay_line<T, Depth>`。

### 10.2 Verilog delay-line：真实 shift register

[`VerilogEmitter.cpp`](../compiler/mlir/lib/Emit/VerilogEmitter.cpp) 生成：

```verilog
pyc_delay_line #(.WIDTH(8), .DEPTH(128)) delay_inst (...);
```

[`pyc_delay_line.v`](../runtime/verilog/pyc_delay_line.v) 使用寄存器数组、同步 reset
循环和 enabled shift。Verilog 不采用 C++ ring index，避免给硬件增加地址状态和
mux。vector delay-line 按 scalar leaf 展开共享控制的实例。

### 10.3 Packed state 的 probe/trace

[`CppEmitter.cpp`](../compiler/mlir/lib/Emit/CppEmitter.cpp) 识别带 layout 属性的
extract，并通过
[`pyc_probe_registry.hpp`](../runtime/cpp/pyc_probe_registry.hpp) 的
`addRegSlice<W, StorageW>` 注册逻辑 state slice。write trace 从 packed
`qNext` 的对应 slice 读取值，实现在
[`pyc_trace_bin.hpp`](../runtime/cpp/pyc_trace_bin.hpp)。

这是 lowering 对 MLIR 已明确 layout 的实现，不是 backend 自行决定 packing 语义。

## 11. 优化效果

### 11.1 单条长链：C++ ring 的收益

8-bit 数据，2,000,000 cycles，baseline/optimized 交替运行，各 5 次取中位数，
`-std=c++17 -O3 -DNDEBUG -march=native`：

| depth | baseline ns/cycle | delay-line ns/cycle | 加速比 | 指令/周期：前 → 后 |
|---:|---:|---:|---:|---:|
| 2 | 26.3991 | 18.8134 | 1.403× | 138.2 → 97.2 |
| 8 | 103.607 | 18.5307 | 5.591× | 529.2 → 96.4 |
| 32 | 576.844 | 18.2922 | 31.535× | 1862.2 → 96.2 |
| 128 | 1,783.62 | 18.7215 | **95.271×** | 7,086.2 → 96.2 |

depth=128 的 logical state bits 仍是 `128 × 8 = 1024`。收益来自 O(depth) 个 C++
状态对象和调度变成一个 O(1) ring primitive。

### 11.2 大型 workload：Stage 1/1.5/2 的综合收益

输入为 `/tmp/xs_core.delay_only.mlir`，命令统一使用
`--emit=cpp --logic-depth=1000`。时间为同机单次测量，用于量级对比。

| 策略 | `reg_count` | state bits | pack groups | packed ops | pycc | C++ header |
|---|---:|---:|---:|---:|---:|---:|
| off | 29,364 | 147,364 | 0 | 0 | 22.40 s | 38,489,749 B |
| structural, preserve=true, pack=0 | 16,060 | 92,225 | 0 | 0 | 21.04 s | 26,939,446 B |
| 默认 structural, preserve=false, pack=192 | 7,171 | 92,171 | 1,995 | 10,869 | 18.06 s | 20,732,146 B |

默认模式相对 off：

- 合并 19,135 个等价 state，记录移除 55,177 bits；
- 创建 6 个最终 delay-line，累计 depth 13；
- packing 处理 10,869 个 state op，形成 1,995 个 pack group；
- 通过 packing 净减少 8,874 个 state primitive；
- 剥离 43,014 个显式观测属性，删除 3 个无功能用途观测 alias；
- header 缩小 46.15%，完整 C++ emission 改善 19.38%。

保留模式和性能模式的 primitive 数不能只归因于观测清理：保留模式还会跳过旧 i1
packing。state bits 的主要下降来自等价状态合并，不是 lane packing。

这里的 `reg_count` 遵循 compile-stats 资源口径：普通或 packed `pyc.reg` 计 1，
`pyc.delay_line depth=N` 计 N。它能防止把长 delay-line 误算成一个逻辑寄存器，但也
不是 C++ state object 数或综合后的 FF cell 数；后两者必须分别检查 IR/backend 和
综合报告。

### 11.3 生成文本和 RTL 状态不能混为一谈

单链示例中，depth=128 的 C++/Verilog 顶层文本曾分别缩小 94.45%/97.89%，但
源码大小不是综合面积。逻辑状态示例：

| 用例 | 优化前 | 优化后 | state bits |
|---|---|---|---:|
| single depth=4, width=8 | 4 reg | 1 delay(depth=4) | 32 → 32 |
| single depth=128, width=8 | 128 reg | 1 delay(depth=128) | 1024 → 1024 |
| duplicate 2×depth=2, width=8 | 4 reg | 1 shared delay(depth=2) | **32 → 16** |
| packed 4+8+3-bit regs | 3 reg | 1 packed reg | 15 → 15 |

## 12. 可观测性和正确性边界

默认 performance policy 有意改变内部物理观测身份，因此团队对齐时应使用以下口径：

| 行为 | 默认性能模式 |
|---|---|
| 模块输入/输出值和周期 | 必须保持 |
| reset/enable/init 语义 | 必须保持 |
| function/instance 功能数据流 | 必须保持 |
| 中间只读 SSA fanout/tap | structural 性能模式改写为 `pyc.delay_tap`；state dependency/写回仍禁止 |
| 内部 reg 的原物理对象和名字 | 不保证 |
| debug/probe/trace/observable-only state | 可删除 |
| state current/next/pending 的独立物理身份 | 不保证 |

因此：

- 功能回归和性能自动化使用默认模式；
- 需要波形逐状态对齐、内部 probe identity 或调试特定寄存器时使用
  `--state-opt-preserve-observability=true`；
- 需要复现最初 cycle-balance 行为时使用 `--state-delay-opt=generated`；
- 需要无状态改写 baseline 时使用 `--state-delay-opt=off`。

## 13. 统计和诊断

新建的 delay-line 和 packed state 会携带来源属性：

```mlir
%q = pyc.delay_line ... {
  depth = 4 : i64,
  pyc.optimized_by = "combine_delay_chains_structural",
  pyc.shared_chain_count = 2 : i64,
  pyc.source_reg_count = 8 : i64
} : i8
```

主要统计字段：

| JSON 字段 | 含义 |
|---|---|
| `state_opt_policy` | off/generated/structural 实际策略 |
| `state_opt_preserve_observability` | 是否保留显式状态身份 |
| `state_opt_pack_width` | 实际 Stage 2 width 上限 |
| `state_opt_regs_seen/generated/pinned` | Stage 0 状态和原始观测边界 |
| `state_opt_merge_candidates` | Stage 0 保守视角的等价状态候选 |
| `state_opt_regs_merged` | 两轮实际合并的 reg 数 |
| `state_opt_reg_bits_removed` | 等价状态合并移除的逻辑 bits |
| `state_opt_merge_rounds` | structural 合并轮数 |
| `state_opt_cascade_regs_merged` | 第二轮新增合并数 |
| `delay_chains_combined` | 成功形成的最大链数量 |
| `delay_chain_regs_combined` | 被 delay-line 替代的 reg 数 |
| `delay_chain_delay_lines_merged` | 完整时序键相同而共享的 delay-line 数 |
| `state_opt_pack_groups` | packed state group 数 |
| `state_opt_packed_state_ops` | 参与 packing 的原 state op 数 |
| `state_opt_state_primitives_removed` | packing 净减少的 primitive 数 |
| `state_opt_pack_bits` | 参与 packing 的逻辑 bit×depth 总量 |
| `state_opt_observability_attrs_stripped` | 性能模式删除的观测属性数 |
| `state_opt_observation_aliases_removed` | 删除的无用途状态 alias 数 |
| `delay_chain_taps_created` | 由中间只读 fanout 形成的固定深度 tap 数 |
| `delay_chain_tap_uses_rewritten` | 被 tap 替换的只读 SSA 使用数 |

`delay_chain_state_reads/writes_before/after` 描述生成 C++ model 每拍需要静态调度的
状态 primitive 数，不是 CPU load/store、MLIR MemoryEffects 或 RTL 物理端口数。

查看 pass 前后 IR：

```bash
pycc input.mlir --emit=none \
  --dump-pass-ir=/tmp/state_ir \
  --dump-pass-ir-filter='strip-state-observability|combine-delay-chains|pack-state-lanes' \
  --dump-pass-ir-phase=both
```

单文件编译统计写入 `<output>.stats.json`；out-dir 模式写入
`compile_stats.json`；`--profile-json` 的 `compile_stats` 也包含同一账本。

## 14. 验证矩阵

当前门禁覆盖：

| Gate | 覆盖 | 结果 |
|---|---|---|
| `state_delay_optimization_smoke.sh` | 默认激进、显式保留、无 marker chain、named/debug、alias control、跨 pack bucket 依赖、probe slice runtime | PASS |
| `check_cascade_state_models.py` | Stage 1.5 两轮合并，397 cycles | C++/Verilog 一致 |
| `check_state_lane_pack_models.py` | reg/delay packing，421 cycles，state bits 72 不变 | C++/Verilog 一致 |
| `check_structural_state_models.py` | off/generated/structural，reset 和 enable stall，383 cycles | 全模型一致 |
| `check_duplicate_shared_models.py` | duplicate history sharing，257 cycles和中途 reset | 全模型一致 |
| `check_verilator_trace.py` | depth=2/4/16，共 192 edges | 匹配独立参考 |
| `check_generated_models.py` | depth=2/8/32/128，各 10,000 cycles | C++/Verilog checksum 一致 |
| `delay_line_diagnostics_smoke.sh` | verifier、clock/comb/depth diagnostics | PASS |

专项 checksum：

```text
cascade     67021053103975750
lane pack   9437495441151636951
structural  7835895766999024842
duplicate   16967771760616252376
```

常用验证命令：

```bash
bash compiler/mlir/test/state_delay_optimization_smoke.sh
bash compiler/mlir/test/delay_line_diagnostics_smoke.sh
python3 verification/delay_line_combine/check_generated_models.py
python3 verification/delay_line_combine/check_duplicate_shared_models.py
python3 verification/delay_line_combine/check_structural_state_models.py
python3 verification/delay_line_combine/check_cascade_state_models.py
python3 verification/delay_line_combine/check_state_lane_pack_models.py
python3 verification/delay_line_combine/check_verilator_trace.py
```

Pack width 性能复现：

```bash
python3 verification/delay_line_combine/run_state_pack_benchmark.py \
  --lanes 128 --cycles 500000 --repeats 3 --widths 0,128,192,256
```

## 15. 修改文件与职责

| 层次 | 文件 | 职责 |
|---|---|---|
| Frontend | [`v5.py`](../compiler/frontend/pycircuit/v5.py)、[`dsl.py`](../compiler/frontend/pycircuit/dsl.py)、[`hw.py`](../compiler/frontend/pycircuit/hw.py) | 生成并传播 cycle-balance provenance |
| Dialect | [`PYCOps.td`](../compiler/mlir/include/pyc/Dialect/PYC/PYCOps.td)、[`PYCOps.cpp`](../compiler/mlir/lib/Dialect/PYC/PYCOps.cpp) | 定义和验证 `pyc.delay_line` |
| Analysis | [`AnalyzeStateOptimizationPass.cpp`](../compiler/mlir/lib/Transforms/AnalyzeStateOptimizationPass.cpp)、[`StateOptimization.cpp`](../compiler/mlir/lib/Transforms/StateOptimization.cpp) | 候选统计、观测边界、状态值归一化和等价证明 |
| Transform | [`StripStateObservabilityPass.cpp`](../compiler/mlir/lib/Transforms/StripStateObservabilityPass.cpp) | 性能模式清理显式状态身份 |
| Transform | [`CombineDelayChainsPass.cpp`](../compiler/mlir/lib/Transforms/CombineDelayChainsPass.cpp) | 等价 state、串行 chain、delay sharing 和两轮统计 |
| Transform | [`PackStateLanesPass.cpp`](../compiler/mlir/lib/Transforms/PackStateLanesPass.cpp) | reg/delay-line lane packing |
| Pipeline | [`pycc.cpp`](../compiler/mlir/tools/pycc.cpp) | 默认策略、pass 顺序、CLI 和统计汇总 |
| Gates | [`CheckClockDomainsPass.cpp`](../compiler/mlir/lib/Transforms/CheckClockDomainsPass.cpp)、[`CheckCombCyclesPass.cpp`](../compiler/mlir/lib/Transforms/CheckCombCyclesPass.cpp)、[`CheckLogicDepthPass.cpp`](../compiler/mlir/lib/Transforms/CheckLogicDepthPass.cpp) | 把 delay-line 视为时序边界并执行 legality 检查 |
| C++ | [`CppEmitter.cpp`](../compiler/mlir/lib/Emit/CppEmitter.cpp)、[`pyc_primitives.hpp`](../runtime/cpp/pyc_primitives.hpp)、[`pyc_probe_registry.hpp`](../runtime/cpp/pyc_probe_registry.hpp)、[`pyc_trace_bin.hpp`](../runtime/cpp/pyc_trace_bin.hpp) | ring runtime、packed slice probe 和 trace |
| Verilog | [`VerilogEmitter.cpp`](../compiler/mlir/lib/Emit/VerilogEmitter.cpp)、[`pyc_delay_line.v`](../runtime/verilog/pyc_delay_line.v) | 可综合 delay-line lowering |
| Tests | [`state_delay_optimization.mlir`](../compiler/mlir/test/state_delay_optimization.mlir)、[`state_observability_performance.mlir`](../compiler/mlir/test/state_observability_performance.mlir)、[`state_optimization_stage15_stage2.mlir`](../compiler/mlir/test/state_optimization_stage15_stage2.mlir)、[`state_delay_tap_codegen.mlir`](../compiler/mlir/test/state_delay_tap_codegen.mlir)、[`state_delay_optimization_smoke.sh`](../compiler/mlir/test/state_delay_optimization_smoke.sh)、[`check_state_delay_tap_models.py`](../compiler/mlir/test/check_state_delay_tap_models.py) | Stage 0-3 正反例、默认策略、tap 双后端 checksum 和运行时 probe gate |
| Verification | [`verification/delay_line_combine/`](../verification/delay_line_combine/README.md) | 跨 backend 等价、benchmark 和复现 |

## 16. 当前没有实现的优化

### 16.1 没有跨寄存器的组合逻辑重定时

当前不会执行：

```text
f(delay(x), delay(y)) → delay(f(x, y))
```

仅改变组合 DAG 的拓扑排序不会减少周期状态；上述变换属于 retiming，必须解决：

- 所有输入 delay 的 clock/reset/enable/depth 一致性；
- 中间 state fanout 和可观测性；
- `new_init = f(init_x, init_y)` 的位精确求值；
- 纯组合、无副作用 op 白名单；
- 比较、mux、截位和四值语义；
- 跨 C++/Verilog 的专用时序等价 gate。

例如两个初值为 0 的寄存器做 equality，新 delay 的 init 应为 1，不能机械沿用 0。
因此 retiming 不能通过“改变计算顺序”或放宽现有 chain matcher 隐式实现。

### 16.2 多抽头 history 的当前边界

当前已实现单个 `pyc.delay_line` 上的多个 `pyc.delay_tap`。canonicalize/CSE 可以
合并相同 depth 的重复 tap，C++ 和 Verilog 共享同一份最大 history。尚未实现的是
把多个 tap 合并为一个多结果 `pyc.delay_taps` op，或基于 tap 数量、depth 和 host
成本自动决定是否值得折叠；当前 chain matcher 采用保守的只读 fanout 条件。

### 16.3 没有基于综合反馈的目标相关 pack cost model

192 是当前 C++ workload 上的默认折中，不代表所有 host、数据宽度和 RTL 目标都最优。
后续可根据 backend、lane width 分布和真实 benchmark 选择不同上限，但不能只以
primitive 数最少为目标。

### 16.4 X-state 和综合资源仍需独立评估

现有动态门禁覆盖 reset 后和中途 reset 的跨后端一致性。reset 前的完整四值一致性属于
pyc4.0 value-model hardening。Yosys/目标综合器的 FF/LUT/memory 数据也应单独报告，
不能从 MLIR op 数或源码字节数推断。

## 17. 用于团队对齐的结论

当前方案可以概括为：

1. `delay_line` 是固定周期历史的一等状态语义，不是省略硬件周期；
2. structural 模式依据状态转移证明，而不是依赖 frontend marker；
3. Stage 1 减少等价状态、形成严格串行历史并共享重复历史；
4. Stage 1.5 用一次有界 canonicalize/CSE 捕获级联机会；
5. Stage 2 将独立窄状态集中为较少的宽 storage，重点降低 C++ primitive 调度；
6. 默认性能模式牺牲内部显式观测身份，但不牺牲端口和周期功能等价；
7. 需要内部调试身份时有明确的 preserve 开关；
8. 只读多抽头 history 已实现；一般 retiming、多结果 tap 原语和目标相关 cost model
   是后续独立阶段。

这套边界使自动化性能测试可以默认获得最大收益，同时保留一个可审计、可回退、
跨 backend 有门禁的安全路径。
