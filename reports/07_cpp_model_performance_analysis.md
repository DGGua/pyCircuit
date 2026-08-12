# pyCircuit 生成 C++ Model 的结构与性能分析

> 调研快照：2026-08-11。分析对象是当前 `pycc` 为 `designs/examples` 生成的 [30 组 C++17 模型](../.pycircuit_out/examples-cpp-models/README.md)。本文沿 `CppEmitter → runtime primitives → testbench` 的真实执行路径解释“模型为什么慢”，不讨论 Verilog 综合后的硬件运行频率。

## 1. 先读结论

生成物通常被口头称为 “C model”，但它实际是一个 **cycle-accurate C++17 模拟器**。它的主要特征是：

1. MLIR 中几乎每个端口、状态和组合中间值都成为一个长期存在的 `Wire<W>` 成员；每个组合 op 又成为 C++ 赋值。
2. 单个时钟周期通常执行两次完整组合求值，并在上升沿、下降沿各走一次 `tick/transfer`。
3. 默认是静态拓扑调度；只有拓扑排序失败的模型才使用 primitive fixed-point 回退。因此 fixed-point 不是普遍慢的主因。
4. `Wire<W>` 按 64 bit word 存储，所以即使 1 bit wire 通常也占 8 bytes。大量临时 SSA 被物化为对象成员后，宿主机内存流量远大于真实寄存器状态量。
5. FIFO 的 two-phase 实现每个有效上升沿复制整个 `Depth` 数组两遍，复杂度是 `O(Depth × Width)`，而不只是处理本周期 push/pop 的一个表项。
6. C++ 编译优化极其重要。本机无 trace 微基准中，`-O2` 相对 `-O0` 提速约 **16–32 倍**。仓库正式构建默认使用 Release/O2；手工直接运行 `g++` 而没有优化，很可能就是“异常慢”的首要原因。
7. 即使使用 O2，大模型仍有结构性成本：`bf16_fmac` 每次 `eval()` 约执行 1182 条生成赋值，一个周期执行两次；它比 Counter 慢不是因为模型做错了，而是当前求值器基本按“整张网表、整周期重算”。
8. VCD/二进制 trace 会额外扫描 probes、比较值并做分配/格式化/I/O；性能测量必须把 trace-off 与 trace-on 分开。

最值得先做的优化，不是改变硬件语义，而是：保证 Release 构建；补齐边沿专用 API；避免无状态变化时的 commit/cache invalidation；把不需要观测的组合临时量降为局部变量；用 MLIR 依赖图生成 dirty-region 求值；将 FIFO 改为只提交变化项。所有调度和状态更新优化都必须先过 C++/Verilog 等价门禁。

## 2. C++ Model 是什么

它不是“把 Verilog 交给另一个仿真器”，而是与 Verilog Emitter 并列的后端：

```text
PYC MLIR
   ├── VerilogEmitter ──► Verilog RTL ──► Verilator / Yosys
   └── CppEmitter ──────► C++ model + C++ runtime ──► native executable
```

硬件语义仍来自 MLIR；C++ Model 只是另一种可执行实现。按照 pyc4.0 Decision 0112，C++ 与 Verilog 必须逻辑等价，不能为了加速只在 C++ 后端偷偷改变 reset、边沿、memory read-during-write 或观察点语义。

当前 `Wire<W>` 是 [`Bits<W>`](../runtime/cpp/pyc_bits.hpp#L106) 的别名，底层是 `std::array<uint64_t, kWords>`，其中 `kWords = ceil(W/64)`（[`pyc_bits.hpp`](../runtime/cpp/pyc_bits.hpp#L111)）。因此当前普通生成值是 **2-state value model**；不要把性能问题误归因于每根 wire 都携带 X/Z。X/Z mask 主要出现在 trace 接口与专门的 value-model 路径。

## 3. 用 Counter 看懂一个模型

Counter 的生成类位于 [`counter.hpp`](../.pycircuit_out/examples-cpp-models/counter/counter/cpp/counter.hpp#L13)，对象中有：

- `clk/rst/enable/count` 四个端口；
- `count__next`、`pyc_add_8`、`pyc_mux_9`、常量等组合中间量；
- 一个 `pyc_reg<8>` runtime primitive；
- probe、模拟统计和运行开关；
- `eval/comb/tick/commit/transfer/step` 等调度 API。

其中真实时序状态只有一个 8 bit counter，但类里共有 17 个 `Wire` 字段，实测 `sizeof(counter) = 224 bytes`。这不等于 224 bytes 全是浪费——端口和可观察值必须存在——但它说明“硬件状态 bit 数”与“C++ 模型对象大小”不是一个量纲。

Counter 的组合求值在 [`counter.cpp`](../.pycircuit_out/examples-cpp-models/counter/counter/cpp/counter.cpp#L67) 中近似等于：

```cpp
count_2 = pyc_reg_7;
pyc_add_8 = count_2 + 1;
pyc_mux_9 = mux(enable, pyc_add_8, count_2);
count__next = pyc_mux_9;
count = count_2;
```

顺序逻辑分成两阶段：

```text
comb()          计算时钟边沿前的 D/控制信号
clk = 1
tick()          检测上升沿，计算 qNext
transfer()      把 qNext 提交给 q
comb()          状态变化后重新计算输出
clk = 0
tick()          做下降沿检测/记账
transfer()      再调用一次 commit
```

生成类自己的 `step()` 也明确执行 `comb → tick → commit → comb`（[`counter.cpp`](../.pycircuit_out/examples-cpp-models/counter/counter/cpp/counter.cpp#L115)）。两次组合求值并非可以随手删掉：第一次为边沿准备 D，第二次让寄存器更新后的输出稳定。优化方向应是准确判断哪些 region 变脏，而不是破坏观察点。

## 4. 30 个样例的静态特征

本次统计扫描了 30 个 `compile_stats.json` 及对应生成的 `.cpp/.hpp`：

| 指标 | 合计 |
|---|---:|
| 生成 `.cpp` 行数 | 8,931 |
| 生成 `.hpp` 行数 | 6,864 |
| 持久化 `Wire/Vec` 字段 | 3,432 |
| `eval_comb_N` 块 | 110 |
| 生成赋值 | 3,337 |
| MLIR op | 3,940 |
| `pyc.reg` | 101 |
| 寄存器 bit | 1,011 |
| raw `new` primitive | 103 |

组合值和生成赋值显著多于顺序状态。代表性模型如下：

| 模型 | Wire 字段 | 组合块 | 赋值 | MLIR op | reg / bit | 最大逻辑深度 | `sizeof(DUT)` |
|---|---:|---:|---:|---:|---:|---:|---:|
| `counter` | 17 | 2 | 16 | 13 | 1 / 8 | 2 | 224 B |
| `npu_node` | 167 | 2 | 161 | 194 | 0 / 0 | 9 | 未测 |
| `sw5809s` | 372 | 11 | 359 | 383 | 12 / 24 | 4 | 7,376 B |
| `traffic_lights_ce` | 261 | 7 | 255 | 303 | 5 / 41 | 13 | 未测 |
| `dodgeball_game` top | 490 | 21 | 485 | 577 | 14 / 98 | 14 | 未测 |
| `bf16_fmac` | 1,189 | 26 | 1,182 | 1,613 | 25 / 244 | 46 | 9,792 B |

`bf16_fmac` 只有 244 bit 显式寄存器，约 31 bytes，却有 9,792-byte DUT 对象；后者约是寄存器净荷的 316 倍。这个比例不能直接叫“浪费率”，因为它还包括端口、组合值、primitive 管理字段和统计字段，但足以证明：**对象足迹主要由模拟器表示法决定，而不是由硬件寄存器 bit 数决定。**

另一个反例是 [`npu_node.hpp`](../.pycircuit_out/examples-cpp-models/fm16/npu_node/cpp/npu_node.hpp#L205)：它含 4 个 `pyc_fifo<32,8>` 以及每个 FIFO 的一组缓存字段，但 [`compile_stats.json`](../.pycircuit_out/examples-cpp-models/fm16/npu_node/cpp/compile_stats.json) 显示 `reg_count=0, mem_count=0`。因此现有 compile stats 不能作为 C++ 仿真工作量估算器，也漏掉了 FIFO runtime 内部状态。

## 5. 微基准：编译优化是第一检查项

### 5.1 方法

- 编译器：`g++ 13.1`，C++17；生成模型单独作为 translation unit，无 LTO；
- 分别使用 `-O0` 与 `-O2`；
- 关闭 VCD、binary trace 和模拟统计；
- 每周期执行与 testbench fast path 等价的两次 `comb`、两次 `tick/transfer`；
- Counter 运行 500 万周期，`sw5809s` 30 万周期，BF16 FMAC 10 万周期；
- 每组运行三次，表中取中位数；未绑核、未隔离系统负载，因此是诊断性微基准，不是正式性能门禁。
- 每组 O0/O2 的最终 checksum 一致；这可防止循环被完全删除，但不能代替 C++/Verilog cycle equivalence。

### 5.2 结果

| 模型 | O0 周期/秒 | O2 周期/秒 | O2/O0 |
|---|---:|---:|---:|
| `counter` | 1.62 M | 42.44 M | 26.2× |
| `sw5809s` | 90.3 K | 1.464 M | 16.2× |
| `bf16_fmac` | 20.7 K | 659 K | 31.9× |

这说明两个不同问题：

1. **异常慢首先查构建方式。** 仓库 CLI 默认 `--profile release`（[`cli.py`](../compiler/frontend/pycircuit/cli.py#L2715)），CMake 使用 Release（[`cli.py`](../compiler/frontend/pycircuit/cli.py#L2481)）；manifest 构建器的 release 明确是 `-O2 -DNDEBUG`（[`build_cpp_manifest.py`](../flows/tools/build_cpp_manifest.py#L85)）。不要用没有 `-O` 的手工编译结果评价生成器。
2. **O2 不是结构优化的替代品。** 同为 O2，Counter 仍比 BF16 FMAC 快约 64 倍。模型每次求值的赋值数、宽位运算、primitive 状态和对象内存流量依旧决定吞吐。

## 6. 为什么仍然慢：按重要性拆解

### 6.1 整张组合网表重复求值

[`CppEmitter.cpp`](../compiler/mlir/lib/Emit/CppEmitter.cpp#L1756) 把组合 op/assign 排成 `eval_comb_pass()`；完整拓扑可用时，`eval()` 直接逐节点执行（[`CppEmitter.cpp`](../compiler/mlir/lib/Emit/CppEmitter.cpp#L2572)）。目前 30 个产物中没有使用 runtime 已提供的 `InputFingerprint`、`EvalGuard` 或 `ChangeDetector`，尽管这些设施存在于 [`pyc_change_detect.hpp`](../runtime/cpp/pyc_change_detect.hpp#L12)。

结果是：即使某周期只改了一个输入 bit，通常仍会执行整个 `eval()`。例如 BF16 FMAC 每次约 1182 条赋值、每周期约两次，相当于约 2364 条生成赋值，尚未计入各个 `Bits` 运算内部工作。

此外，常量也被保存为字段并在每次 eval 时重新赋值。Counter 每次都会重写 0、1 等常量（[`counter.cpp`](../.pycircuit_out/examples-cpp-models/counter/counter/cpp/counter.cpp#L75)）。O2 能消去一部分，但跨 translation unit、调试构建和复杂别名会限制优化器。

### 6.2 中间 SSA 全部“对象化”

硬件 IR 的 SSA 临时值本来可以是 C++ 局部变量，甚至被编译器完全消去；当前生成器为了命名、连接、probe 和 primitive 引用，广泛把它们变成 DUT 成员。`Wire<1>` 与 `Wire<64>` 都至少含一个 64-bit word（[`pyc_bits.hpp`](../runtime/cpp/pyc_bits.hpp#L107)）。

影响包括：

- 更大的 DUT 和更高的 cache footprint；
- 每个 op 更容易产生真实 load/store，而不是只留在寄存器；
- 生成头文件、符号和调试信息膨胀；
- 大模型的 instruction cache、编译时间与内联压力增大。

应注意 probe/观察点是 pyc4.0 合同，不能简单删除所有字段。合理做法是通过 MLIR use/observation analysis，把“端口、状态、primitive 绑定值、manifest 指定 probe”保留为成员，其余无跨函数生命期的组合 SSA 降为局部临时量。

### 6.3 testbench 的边沿 API 没有真正接上

runtime testbench 已能检测 DUT 是否提供 `tick_posedge()` / `tick_negedge()`；否则两边沿都回退到普通 `tick()`（[`pyc_tb.hpp`](../runtime/cpp/pyc_tb.hpp#L64)）。`pyc_reg` 本身也有更轻的 `posedge_tick_compute()` 和 `negedge_update()`（[`pyc_primitives.hpp`](../runtime/cpp/pyc_primitives.hpp#L87)）。

但 30 个生成模型中，顶层边沿专用 API 出现次数为 0。fast path 因而在上升沿和下降沿各调用一次通用 `tick()` 与 `transfer()`（[`pyc_tb.hpp`](../runtime/cpp/pyc_tb.hpp#L332)）。生成的 `transfer()` 又只是 `tick_commit()` 的别名（[`CppEmitter.cpp`](../compiler/mlir/lib/Emit/CppEmitter.cpp#L2726)）。

寄存器自身能快速发现“没有 posedge/pending”，所以这不一定是最大热点；但大层次、多 primitive 模型仍付出了遍历、函数调用和缓存失效成本。更严重的是 `tick_commit()` 会无条件把 stateful instance/FIFO/memory 的 eval cache 标为无效（[`CppEmitter.cpp`](../compiler/mlir/lib/Emit/CppEmitter.cpp#L2688)），下降沿的无变化 `transfer()` 也会造成下一次无法命中缓存。

### 6.4 FIFO 每周期复制整个存储数组

`pyc_fifo<Width,Depth>` 同时保存 `storage_[Depth]` 和 `storageNext_[Depth]`。每个有效上升沿：

1. `tick_compute()` 把全部 current storage 复制到 next storage（[`pyc_primitives.hpp`](../runtime/cpp/pyc_primitives.hpp#L253)）；
2. push 最多只改 next storage 的一个表项；
3. `tick_commit()` 又把全部 next storage 复制回 current storage（[`pyc_primitives.hpp`](../runtime/cpp/pyc_primitives.hpp#L276)）。

因此一次 push/pop 的宿主机工作量从应有的 `O(1)` 变成 `O(Depth × ceil(Width/64))`，对象中还存了两份 FIFO 数据。深 FIFO 或宽 packet 会快速放大这项成本。这是已由 runtime 代码确认的结构性问题，不依赖微基准猜测。

### 6.5 primitive cache 有成本，但活跃负载未必获益

Emitter 为 FIFO 输入保存值、fingerprint、version、seen-version 和 valid 等缓存元数据，`npu_node` 的字段展开可以直接看到（[`npu_node.hpp`](../.pycircuit_out/examples-cpp-models/fm16/npu_node/cpp/npu_node.hpp#L205)）。缓存逻辑由 [`CppEmitter.cpp`](../compiler/mlir/lib/Emit/CppEmitter.cpp#L1986) 生成。

在 `sw5809s` 活跃输入微基准上启用 `PYC_SIM_STATS=1`，10,000 周期得到：

```text
primitive_eval_calls = 320000   # 16 FIFOs × 每周期 2 次 comb
primitive_cache_skips = 0
fallback_iterations = 0
```

这只能证明这个 workload 中缓存没有命中，不能推导“缓存永远无用”。原因包括输入每周期变化、FIFO 状态变化，以及每次 transfer 后 cache invalidation。正确方向是按 primitive/region 成本与实测命中率选择缓存策略，而不是全局一刀切。

### 6.6 fixed-point 是局部风险，不是普遍根因

当 primitive 也无法形成完整拓扑顺序时，Emitter 才生成 fixed-point：最多迭代 primitive 数，每轮执行 primitive group 和整次 `eval_comb_pass()`，直到没有变化（[`CppEmitter.cpp`](../compiler/mlir/lib/Emit/CppEmitter.cpp#L2500)）。可选的 SCC worklist 快路径由 `PYC_SIM_FAST=1` 控制（[`CppEmitter.cpp`](../compiler/mlir/lib/Emit/CppEmitter.cpp#L2601)）。

当前 30 个样例中只有：

- `dodgeball_game/lab_final_top`；
- `traffic_lights_ce_pyc/traffic_lights_ce`；

生成了 `eval_fixpoint_fallback_path()`，且没有一个样例生成 `eval_fast_scc_path()`。`sw5809s` 的统计为 `fallback_iterations=0`。因此对于大多数当前样例，慢应先从全网表求值、边沿调度、FIFO 和构建 profile 查，而不是先优化 fixed-point。

### 6.7 trace 可能掩盖核心模拟性能

VCD `dump()` 每次遍历所有已注册 signal，构造/重用一个动态 vector、读取并比较，再通过 ostream 输出变化（[`pyc_vcd.hpp`](../runtime/cpp/pyc_vcd.hpp#L53)）。binary trace 的 `sampleDelta()` 会为 value、known mask、z mask 构造多个 vector（[`pyc_trace_bin.hpp`](../runtime/cpp/pyc_trace_bin.hpp#L363)），每个观察 phase 还要扫描 probes。

所以 trace-on 的瓶颈可能主要是分配、序列化和 I/O，而不是 DUT eval。正式测量至少要报告：

- trace-off：模型内核吞吐；
- trace-on，固定 probe 集合与时间窗口：可观测调试吞吐；
- trace 文件字节数与 probe 数。

仓库已有 bounded VCD window 接口（[`pyc_tb.hpp`](../runtime/cpp/pyc_tb.hpp#L162)），性能测试应默认关闭 trace，调试时尽量缩小窗口和 probe 集合。

### 6.8 其他次要问题

- **宽位运算**：`Bits<W>` 的操作按 word 循环；ARM64 有 NEON 辅助，当前 x86 路径主要依赖编译器优化。特别宽的除法还有算法级成本。它会放大宽 datapath 模型，但不是 Counter 慢的解释。
- **raw ownership**：30 个模型共生成 103 个 raw `new`，未生成对应模型析构函数。它是重复构造 DUT、批量测试与复制安全问题，不是稳定态每周期第一热点。submodule 已使用 `unique_ptr`，local reg/sync-mem 也应统一 RAII 或原位存储。
- **常驻统计/控制字段**：每个模型都带 stats、环境变量开关和 probe 方法，即使不用。这更偏对象/代码体积问题，运行时统计关闭后不是主要热路径。
- **生成函数分片**：对大模型有助于编译稳定性，却可能阻碍跨函数/跨 TU 内联。可把 LTO 或明确的性能 profile 作为补充实验，但不要在没有 perf 数据时假定它一定收益。

## 7. 优化路线：从低风险到结构性改造

### P0：先排除使用问题

1. 所有性能数据使用 `pycircuit build --profile release` 或 manifest builder 的 release profile。
2. 同时记录编译器、flags、是否 LTO、CPU、cycles、checksum；不要只看 wall time。
3. 默认关闭 trace 和 `PYC_SIM_STATS`；诊断缓存/fallback 时再单独开启。
4. 在 CI 建立 `counter / FIFO / bf16_fmac / hierarchy / multi-clock` 五类小型吞吐基线。

### P1：低风险 emitter/runtime 优化

1. **生成边沿专用 API**：top-level `tick_posedge/tick_negedge` 下沉调用 primitive 的专用方法。
2. **只在状态真正提交时 invalidation**：区分“调用过 transfer”和“state version 改变”。
3. **常量提升**：能折叠的常量用 `constexpr`/literal，避免每次 eval 写常量成员。
4. **缓存成本模型**：用 `primitive_cache_skips / eval_calls` 决定是否为该类 primitive 生成版本缓存。
5. **性能构建档**：在 O2 基础上 A/B 测试 O3、LTO、`-march=native`，收益和可移植性分别记录。
6. **trace 缓冲复用**：复用 value/mask buffer，减少 probe 级 vector 分配，批量输出。

### P2：收益更大的结构优化

1. **ephemeral SSA lowering**：仅把端口、状态、primitive 绑定值和观察点保留为字段，其余组合值变为局部变量。
2. **dirty-region/event-driven eval**：利用 MLIR dependency graph 把组合网表分 region；输入或状态 version 未变时跳过不相关 region。
3. **跨周期共享**：若边沿前输入与依赖状态没有变化，跳过 pre-comb；提交后只重算受变更寄存器影响的 region。
4. **FIFO 增量提交**：two-phase 阶段只记录 `rd/wr/count` 和可选的一个 write transaction，不复制整个数组；reset 可采用 generation/lazy clear 或单独慢路径。
5. **实例层次调度**：把 submodule input/output version 与内部 state version 分开，避免父层每次 commit 都让所有有状态子模块失去缓存。

### P3：工程质量

1. local primitive 改为原位对象或 `std::unique_ptr`，并显式禁止危险复制/移动；
2. 为模型额外生成 `simulation_cost.json`，至少统计 persistent wires、primitive 类型/深度、预计 eval nodes、fallback/SCC 路径、probe 数；
3. 将 cycles/s、eval calls/cycle、cache hit ratio、fallback iterations/cycle、DUT bytes、trace bytes/cycle 纳入统一 perf dashboard。

## 8. Gate-first：怎么优化而不改错语义

性能修改必须先准备正确性门禁。至少覆盖：

| 改动 | 必须验证的语义 |
|---|---|
| 删除/跳过某次 comb | pre-edge D、post-commit output、Decision 0113 观察点一致 |
| 边沿专用 tick | posedge/negedge、多时钟、clock phase、reset 行为一致 |
| FIFO 增量提交 | 同周期 push+pop、full/empty、reset、old-data 行为一致 |
| SSA 局部化 | probe manifest、层次路径、可观察信号不丢失 |
| cache/version | 输入变化、内部状态变化、reset invalidation 不漏传播 |
| SCC/fixed-point | 组合环诊断、收敛性与拓扑模型结果一致 |

建议门禁顺序是：

```text
MLIR verifier / pass invariant
        ↓
C++ model 单元测试
        ↓
C++ ↔ Verilog/Verilator cycle-by-cycle equivalence
        ↓
Counter、FIFO old-data、multi-clock、reset、hierarchy、X/Z trace 回归
        ↓
trace-off 与 trace-on 性能门禁
```

这也决定了优化应主要落在 dialect/pass 所表达的依赖和观察语义上；Emitter 可以改变表示与调度，但不能成为唯一知道“何时可跳过计算”的语义孤岛。

## 9. 建议的第一轮实施顺序

若目标是在较短时间内获得可信提升，建议：

1. 建立固定 benchmark harness，锁定 Release/O2 和 trace-off 基线；
2. 增加 `tick_posedge/tick_negedge` 生成及状态版本统计，消除下降沿无效 commit/invalidation；
3. 用 Counter、BF16 FMAC 验证 ephemeral SSA/constant lowering 对对象大小、load/store 和吞吐的收益；
4. 用 `npu_node/sw5809s` 实现并验证 FIFO 增量提交；
5. 再做 MLIR dependency-region/dirty scheduling，因为它潜力最大，但观察点和层次语义风险也最高；
6. 最后按实际 trace profile 优化 VCD/binary trace，不把 I/O 提升与 eval 内核提升混在一个数字里。

## 10. 最终判断

这些生成模型慢，不是单一 bug，也不能简单归咎于“C++ 比 Verilog 慢”。当前实现本质上是一个直接、保守、容易对应 MLIR 语义的 cycle simulator：把大量 IR 值物化为字段，并在每个观察阶段重算大部分网表。这种结构有利于早期正确性和调试，却牺牲了宿主机 locality、增量求值和 primitive 状态更新效率。

就当前证据，优先级可以概括为：

```text
错误的 O0 构建
    > 全网表每周期两次求值
    > FIFO 全数组 two-phase copy / 无效边沿遍历与 cache invalidation
    > 活跃负载下低命中缓存的元数据成本
    > trace 扫描、分配与 I/O（仅 trace-on）
    > 少数模型的 fixed-point 回退
    > raw ownership 等工程问题
```

其中第一项可立即规避；中间几项需要 emitter/runtime 优化；dirty-region 与 SSA lifetime 优化需要 MLIR 层参与。后续评价任何方案时，应同时给出 cycle 等价结果、cycles/s、eval 次数、cache 命中和对象/trace 体积，避免只靠生成代码行数或 `compile_stats.json` 判断效率。
