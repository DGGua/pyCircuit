# Delay-line 周期对齐寄存器链优化

本文说明 pyCircuit 中 `pyc.delay_line` 和 `pyc-combine-delay-chains` Pass 的
设计、实现、验证方法及当前状态，供代码评审和后续维护使用。详细实验报告见
[reports/delay_line/](../reports/delay_line/README.md)，原始产物与复现脚本见
[verification/delay_line_combine/](../verification/delay_line_combine/README.md)。

## 1. 背景与结论

V5 cycle-aware frontend 会自动对齐参与同一运算的信号周期。例如：

```text
c = a@0 + b@4

a ─▶ reg0 ─▶ reg1 ─▶ reg2 ─▶ reg3 ─┐
b@4 ────────────────────────────────┴─▶ add ─▶ c
```

原实现为 `a` 生成四个独立 `pyc.reg` 及配套 wire/alias。RTL 语义正确，但生成
C++ 模型也产生四个状态对象和逐级 `tick_compute()`/`tick_commit()` 调度；当
depth 增至 32、128 时，每个模拟周期的工作量随深度增长。

本功能将可证明安全的自动周期对齐链提升为一等 MLIR 状态操作：

```mlir
%q = pyc.delay_line %clk, %rst, %en, %input, %init
     {depth = 4 : i64, pyc.generated = "cycle_balance"} : i8
```

其结果是：

- C++ runtime 使用环形缓冲区，正常 enabled edge 由 O(depth) 降为 O(1)；
- Verilog 保留可综合的 N 级 shift-register 时序；
- 单链仍按 `depth × width` 统计真实状态，不把一个 MLIR op 当作一个 FF；
- 完全相同的多条历史可以共享，从而消除重复状态。

8-bit、depth=128 的生成 C++ 模型实测由 1783.62 ns/cycle 降至
18.7215 ns/cycle，加速 95.271×。该数据是 C++ 仿真结果，不是综合面积结果。

## 2. `pyc.delay_line` 的语义

操作定义在
[`PYCOps.td`](../compiler/mlir/include/pyc/Dialect/PYC/PYCOps.td)，输入为
`clk, rst, en, next, init`，输出为最终一级 `q`。`depth=N` 等价于 N 个共享
clock/reset/enable/init 的串行 `pyc.reg`：

```text
q0 <= next
q1 <= q0
...
q[N-1] <= q[N-2]
q = q[N-1]
```

准确语义如下：

1. 延迟按 enabled positive edge 计数；`en=0` 时整条历史保持；
2. reset 优先于 enable，reset edge 将全部历史位置写为 `init`；
3. `tick_compute()` 只计算下一状态，`tick_commit()` 后状态才可见；
4. scalar 和 vector 数据均可使用；
5. `next/init/q` 类型必须一致，`depth` 必须存在且大于 1。

Verifier 位于
[`PYCOps.cpp`](../compiler/mlir/lib/Dialect/PYC/PYCOps.cpp)。depth=1 继续由
`pyc.reg` 表示，避免两种操作表达同一种基础状态。

### 2.1 与 FIFO 的区别

Delay-line 没有 ready/valid、full/empty、backpressure 或独立的入队/出队。它是
按共同 enable 推进的固定深度历史，因此不是 `FIFO(max_depth)`。

### 2.2 一个 op 不等于一个 FF

`pyc.delay_line depth=N` 是紧凑 IR 表示，仍承载 N 级状态。width=W 时：

```text
logical register count = N
logical state bits     = N × W
```

单链合并减少的是 IR、生成文本和模拟调度；只有第 6 节的 duplicate sharing 才
可能减少实际重复状态。

## 3. 端到端处理流程

```text
CycleAwareDomain.delay_to()
  │  pyc.generated = "cycle_balance"
  ▼
raw wire / assign / alias / pyc.reg chain
  │  eliminate-wires → eliminate-dead-state
  ▼
pyc-combine-delay-chains
  │  legality proof → maximal chain → duplicate sharing
  ▼
pyc.delay_line {depth=N}
  ├─ clock/comb/logic-depth gates + compile statistics
  ├─ C++ emitter     → pyc_delay_line<Width, Depth>
  └─ Verilog emitter → pyc_delay_line #(.WIDTH, .DEPTH)
```

在 [`pycc.cpp`](../compiler/mlir/tools/pycc.cpp) 中，Pass 位于 wire/dead-state 清理
之后和组合规范化、clock/comb/logic-depth gates 之前。`--combine-delay-chains`
默认开启；用 `--combine-delay-chains=false` 可生成 baseline。

这符合 gate-first 原则：状态语义先进入 Dialect 和 MLIR Pass，各项分析显式认识
新操作，后端只负责 lowering，而不是根据私有名字修补语义。

## 4. 如何判别可改写的多级状态模式

核心代码为
[`CombineDelayChainsPass.cpp`](../compiler/mlir/lib/Transforms/CombineDelayChainsPass.cpp)。
识别策略是保守证明：任何安全条件不成立，都保留原始寄存器链。

### 4.1 所有权：只处理 frontend 自动生成状态

V5 `delay_to()` 为平衡 wire、reg 和 alias 传播：

```mlir
{pyc.generated = "cycle_balance"}
```

Pass 只处理该精确 marker，不根据 `_v5_bal_*` 名字猜测。这样用户同名寄存器不会
被误改写，重命名也不会破坏识别。`EliminateWiresPass` 在 wire→alias 时保留属性，
保证 marker 能到达合并 Pass。

### 4.2 从 tail 反向寻找最大链

Pass 收集所有带 marker 且没有 `pyc.debug_keep` 的 `pyc.reg`，逆序选择 tail，
沿 consumer 的 `next` 向前匹配 predecessor。每条连接必须满足：

1. 中间只经过同 marker、无 `debug_keep` 的透明 `pyc.alias`；
2. predecessor 也是同 marker、无 `debug_keep` 的 `pyc.reg`；
3. predecessor q 与 tail q 类型一致；
4. clock 和 reset 为相同 SSA value；
5. enable 和 init 语义等价；
6. predecessor q 及路径上每个 alias 都只有当前链这一处使用。

反向搜索停止时得到最大安全链。链长度成为 `depth`；head 的 `next` 成为输入，
head 的 controls 成为 delay-line controls；tail q 的使用全部替换为 delay q。

### 4.3 one-use/fanout 是语义门

以下链不能合并：

```mlir
%q0 = pyc.reg ... %input ... {pyc.generated = "cycle_balance"} : i8
%q1 = pyc.reg ... %q0    ... {pyc.generated = "cycle_balance"} : i8
return %q0, %q1 : i8, i8
```

`%q0` 是可观察的中间 tap，而当前 delay-line 只暴露末端 q。故每段路径必须满足
`hasOneUse()`；中间 q 被 return、probe、其他运算或额外 alias 使用时立即停止。

同样不会合并的情况还有：未标记链、任一级 `debug_keep`、控制不一致、类型不一致
以及长度小于 2。当前实现宁可漏掉收益，也不放宽语义证明。

### 4.4 enable/init 的有限等价

前端可能为不同级分别创建值相同的常量，例如 `%en0` 和 `%en1` 都是
`pyc.constant 1`，但不是同一 SSA value。`equivalentValue()` 因此：

1. 先剥离生成 alias；
2. 接受相同 SSA value；
3. 或接受类型相同、两端均为 `pyc.constant` 且 `value` 属性相同的值。

它不会证明任意动态表达式等价，避免误合并，也避免在此 Pass 内重做通用 CSE。

## 5. 改写算法与数据结构

主要数据结构为：

```cpp
struct ChainLink {
  pyc::RegOp predecessor;
  llvm::SmallVector<pyc::AliasOp> aliasesFromConsumerToProducer;
};

llvm::SmallVector<pyc::RegOp> chainFromTail;
llvm::DenseSet<Operation *> erased;
```

- `ChainLink` 保存 predecessor 和一条边上的透明 alias；
- `SmallVector` 保存候选、tail→head 链及 alias 路径；
- `DenseSet` 记录已删除 op，防止逆序遍历访问失效节点。

算法概要：

```text
for tail in reverse(generated_regs):
    skip tail if already erased
    walk backward while matchPredecessor() succeeds
    skip chains shorter than 2
    create delay_line(depth=chain length, input=head.next)
    replace tail.q uses with delay.q
    erase consumer → aliases → predecessor
```

从 consumer 端删除，会先释放离 consumer 最近的 alias，再释放 predecessor，避免
删除仍有使用的 producer。

## 6. 相同 delay-line 的共享

### 6.1 共享条件

`pyc.delay_line` 有 MemoryEffects，不能依赖普通 CSE 合并。Pass 显式比较完整时序
键，只有下列字段全部等价才共享：

```text
(depth, result type, clock, reset, enable, input, init)
```

depth 和类型必须相同，clock/reset 必须是同一 SSA value，enable/input/init 使用
第 4.4 节的有限等价规则；两端还必须由 `cycle_balance` 生成且没有
`debug_keep`。匹配后，将后一个 q 的使用替换为第一个 q，再删除重复状态。

### 6.2 正确性理由

两个 delay-line 若在相同 clock edge 上采样相同 input，并具有相同 reset、enable、
init、depth 和类型，则初始/复位状态和每次状态转移都相同。由状态转移归纳可知，
每个可观察周期的 q 均相同，因此消费者可以共享同一份历史。

例如同一个 `a@0` 两次对齐到周期 2：

```text
优化前：a → reg0 → reg1 → c0    共 4×8 = 32 bit
        a → reg2 → reg3 → c1

优化后：a → delay(depth=2) ─┬→ c0  共 2×8 = 16 bit
                             └→ c1
```

这是实际重复状态消除，不是“一个 delay op 只算一个寄存器”。

### 6.3 当前复杂度

共享阶段对 M 个生成 delay-line 做确定性双层比较，复杂度为 O(M²)。周期平衡链
通常有限，当前实现优先简单和可审阅；若真实大型设计显示该阶段成为热点，可将
规范化的完整键放入 `DenseMap`，降为期望 O(M)。

## 7. 后端表示

### 7.1 C++：环形缓冲

原模型为每一级创建一个 `pyc_reg` 并逐个调度；优化后只生成：

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

正常 enabled edge 只读一个历史槽、写一个槽并推进 `head`，因此为 O(1)。reset
需要 `stages.fill(init)` 以保持所有级同时复位，仍为 O(depth)。scalar/vector
分别使用 `pyc_delay_line<Width,Depth>` 和 `pyc_vec_delay_line<T,Depth>`。

### 7.2 Verilog：可综合 shift register

[`VerilogEmitter.cpp`](../compiler/mlir/lib/Emit/VerilogEmitter.cpp) 生成：

```verilog
pyc_delay_line #(.WIDTH(8), .DEPTH(128)) delay_inst (...);
```

[`pyc_delay_line.v`](../runtime/verilog/pyc_delay_line.v) 使用寄存器数组、同步 reset
循环和 enabled shift 循环。Verilog 不采用 C++ ring index，避免向硬件引入额外
地址状态和 mux。vector delay-line 按 scalar leaf 展开多个共享控制的实例。

## 8. 修改文件与职责

| 层次 | 文件 | 职责 |
|---|---|---|
| Frontend | [`v5.py`](../compiler/frontend/pycircuit/v5.py) | `delay_to()` 标记自动周期平衡状态 |
| Frontend | [`dsl.py`](../compiler/frontend/pycircuit/dsl.py)、[`hw.py`](../compiler/frontend/pycircuit/hw.py) | wire/reg/alias 接收并传播 `pyc.generated` |
| Dialect | [`PYCOps.td`](../compiler/mlir/include/pyc/Dialect/PYC/PYCOps.td)、[`PYCOps.cpp`](../compiler/mlir/lib/Dialect/PYC/PYCOps.cpp) | 定义 `pyc.delay_line` 并验证 type/depth |
| Transform | [`CombineDelayChainsPass.cpp`](../compiler/mlir/lib/Transforms/CombineDelayChainsPass.cpp) | 模式识别、最大链改写和 duplicate sharing |
| Registration | [`Passes.h`](../compiler/mlir/include/pyc/Transforms/Passes.h)、[`CMakeLists.txt`](../compiler/mlir/CMakeLists.txt)、[`pyc-opt.cpp`](../compiler/mlir/tools/pyc-opt.cpp) | 声明、构建和注册 Pass |
| Pipeline | [`pycc.cpp`](../compiler/mlir/tools/pycc.cpp) | 默认启用 Pass，输出 primitive、probe 和统计 |
| Gates | [`CheckClockDomainsPass.cpp`](../compiler/mlir/lib/Transforms/CheckClockDomainsPass.cpp)、[`CheckCombCyclesPass.cpp`](../compiler/mlir/lib/Transforms/CheckCombCyclesPass.cpp)、[`CheckLogicDepthPass.cpp`](../compiler/mlir/lib/Transforms/CheckLogicDepthPass.cpp)、[`CombDepGraph.cpp`](../compiler/mlir/lib/Transforms/CombDepGraph.cpp) | 把 delay-line 作为时序边界并检查 clock sampling |
| Cleanup/stats | [`EliminateDeadStatePass.cpp`](../compiler/mlir/lib/Transforms/EliminateDeadStatePass.cpp)、[`EliminateWiresPass.cpp`](../compiler/mlir/lib/Transforms/EliminateWiresPass.cpp)、[`CollectCompileStatsPass.cpp`](../compiler/mlir/lib/Transforms/CollectCompileStatsPass.cpp) | dead state、marker 保留、按 depth 统计状态 |
| C++ | [`CppEmitter.cpp`](../compiler/mlir/lib/Emit/CppEmitter.cpp)、[`pyc_primitives.hpp`](../runtime/cpp/pyc_primitives.hpp) | 声明/构造/调度 scalar/vector ring primitive |
| Verilog | [`VerilogEmitter.cpp`](../compiler/mlir/lib/Emit/VerilogEmitter.cpp)、[`pyc_delay_line.v`](../runtime/verilog/pyc_delay_line.v) | 参数化 scalar/vector-leaf lowering |
| Tests | [`delay_line_combine.mlir`](../compiler/mlir/test/delay_line_combine.mlir)、[`delay_line_diagnostics_smoke.sh`](../compiler/mlir/test/delay_line_diagnostics_smoke.sh)、[`delay_line_verify_valid.mlir`](../compiler/mlir/test/delay_line_verify_valid.mlir)、[`delay_line_verify_invalid.mlir`](../compiler/mlir/test/delay_line_verify_invalid.mlir)、[`test_delay_line_combine.py`](../tests/v5/test_delay_line_combine.py) | Dialect、Pass 正反例、优化统计和 marker 回归 |
| Verification | [`verification/delay_line_combine/`](../verification/delay_line_combine/README.md) | runtime、Icarus、Verilator、sharing、性能和 artifacts |

Probe manifest/C++ probe registry 会过滤 `_v5_bal_*` 生成临时名，但仍将最终
delay-backed 输出分类为 state。

### 8.1 如何查看一次优化做了什么

`pyc-combine-delay-chains` 会直接在新结点上保留优化来源，因此通过 pass IR dump
即可定位具体的改写结果：

```mlir
%q = pyc.delay_line %clk, %rst, %en, %a, %init {
  depth = 4 : i64,
  pyc.generated = "cycle_balance",
  pyc.optimized_by = "combine_delay_chains",
  pyc.shared_chain_count = 2 : i64,
  pyc.source_reg_count = 8 : i64
} : i8
```

这个例子表示两条相同的 depth=4 链共享为一个 delay line，survivor 一共代表 8 个
原始 `pyc.reg`。查看某次编译的 before/after：

```bash
pycc input.pyc --emit=none \
  --dump-pass-ir=/tmp/delay_ir \
  --dump-pass-ir-filter='combine-delay-chains' \
  --dump-pass-ir-phase=both
```

Pass 同时在函数上写入 `pyc.stats.delay_chain_*` 优化账本，`pycc` 会将其跨函数
汇总到 stderr、单文件 `<output>.stats.json`、out-dir `compile_stats.json` 和
`--profile-json` 的 `compile_stats` 对象：

| JSON 字段 | 含义 |
|---|---|
| `delay_chains_combined` | 成功改写的最大 generated 链数量 |
| `delay_chain_regs_combined` | 被这些链替代的 `pyc.reg` 数量 |
| `delay_chain_aliases_removed` | 随链删除的 generated alias 数量 |
| `delay_chain_delay_lines_created` | sharing 前创建的 delay line 数量 |
| `delay_chain_delay_lines_merged` | 因完整时序 key 相同而合并的 delay line 数量 |
| `delay_chain_state_reads_before/after` | C++ 模型 compute/sample 阶段的状态 primitive 静态调度数 |
| `delay_chain_state_writes_before/after` | C++ 模型 commit/update 阶段的状态 primitive 静态调度数 |

例如两条共享的 depth=2 链会报告：

```text
delay_chain={chains:2, regs:4, aliases:2, created:2, merged:1,
             reads:4->1, writes:4->1}
```

这里的 read/write 不是 CPU load/store 指令数，不是 MLIR `MemoryEffects` 数量，也不是
Yosys 综合后的物理读写端口；它描述的是生成 C++ model 每拍需要调度多少个状态
primitive。实际 CPU 指令和耗时仍应使用 `perf`，综合资源仍应使用 Yosys `stat`。

## 9. 优化前后示例

### 9.1 MLIR

```mlir
// before：N 个 reg/alias
%q0 = pyc.reg %clk, %rst, %en0, %a,  %init0
      {pyc.generated = "cycle_balance"} : i8
%a0 = pyc.alias %q0 {pyc.generated = "cycle_balance"} : i8
%q1 = pyc.reg %clk, %rst, %en1, %a0, %init1
      {pyc.generated = "cycle_balance"} : i8

// after：一个保留深度的状态操作
%q = pyc.delay_line %clk, %rst, %en, %a, %init
     {depth = 4 : i64, pyc.generated = "cycle_balance",
      pyc.optimized_by = "combine_delay_chains",
      pyc.shared_chain_count = 1 : i64,
      pyc.source_reg_count = 4 : i64} : i8
```

实际快照：

- [single depth=4 before/after](../verification/delay_line_combine/artifacts/single_depth4_before.mlir)
  与 [after](../verification/delay_line_combine/artifacts/single_depth4_after.mlir)；
- [duplicate depth=2 before/after](../verification/delay_line_combine/artifacts/duplicate_depth2_before.mlir)
  与 [after](../verification/delay_line_combine/artifacts/duplicate_depth2_after.mlir)。

### 9.2 生成模型

```text
C++ before: N × pyc_reg object + N × compute/commit call
C++ after:  1 × pyc_delay_line object + 1 × compute/commit call

Verilog before: N × pyc_reg instance + intermediate wires
Verilog after:  1 × pyc_delay_line #(.WIDTH(W), .DEPTH(N)) instance
```

## 10. 最终效果

基准为 8-bit 数据，每个样本 2,000,000 模拟周期；baseline/optimized 交替运行，
各重复 5 次取中位数，使用同一编译器和
`-std=c++17 -O3 -DNDEBUG -march=native`。同 depth 的全部 checksum 一致。

| depth | baseline ns/cycle | optimized ns/cycle | 加速比 | 指令/周期：前 → 后 |
|---:|---:|---:|---:|---:|
| 2 | 26.3991 | 18.8134 | 1.403× | 138.2 → 97.2 |
| 8 | 103.607 | 18.5307 | 5.591× | 529.2 → 96.4 |
| 32 | 576.844 | 18.2922 | 31.535× | 1862.2 → 96.2 |
| 128 | 1783.62 | 18.7215 | **95.271×** | 7086.2 → 96.2 |

| depth | C++ bytes：前 → 后 | Verilog bytes：前 → 后 |
|---:|---:|---:|
| 2 | 5,646 → 4,957 | 1,744 → 1,202 |
| 8 | 9,528 → 4,957 | 4,318 → 1,202 |
| 32 | 25,263 → 4,962 | 14,798 → 1,207 |
| 128 | 89,475 → 4,967 | 57,552 → 1,212 |

depth=128 的 C++/Verilog 顶层文本分别缩小 94.45%/97.89%，但源码大小不是
综合面积指标。逻辑状态结果为：

| 用例 | 优化前 | 优化后 | state bits |
|---|---|---|---:|
| single depth=4, width=8 | 4 reg | 1 delay(depth=4) | 32 → 32 |
| single depth=128, width=8 | 128 reg | 1 delay(depth=128) | 1024 → 1024 |
| duplicate 2×depth=2, width=8 | 4 reg | 1 shared delay(depth=2) | **32 → 16** |

原始数据见
[depth=2](../verification/delay_line_combine/artifacts/benchmark_depth2_results.json)、
[8](../verification/delay_line_combine/artifacts/benchmark_depth8_results.json)、
[32](../verification/delay_line_combine/artifacts/benchmark_depth32_results.json) 和
[128](../verification/delay_line_combine/artifacts/benchmark_depth128_results.json)。

## 11. 验证方法与结果

| 层次 | 覆盖 | 结果 |
|---|---|---|
| Dialect | scalar/vector 合法；缺失/非法 depth | 合法通过，非法被拒绝 |
| Pass 正例 | scalar/vector 最大生成链、相同链共享 | 通过 |
| Pass 反例 | fanout、未标记、`debug_keep` | 保持原结构 |
| C++ primitive | reset、连续输入、enable hold、reset 优先、两阶段更新 | 通过 |
| Icarus | Verilog primitive smoke test | 通过 |
| Verilator trace | depth=2/4/16，各 64 个含 stall/reset 的 edge | 匹配独立参考 |
| 生成模型 | depth=2/8/32/128，C++ vs Verilator，各 10,000 周期 | checksum 一致 |
| sharing 专项 | baseline/optimized C++/Verilator，257 周期含中途 reset | 四模型逐周期匹配参考 |
| pyc4 gates | run-id `delay-line-combine-20260812-final3` | G1/G2 通过 |

Sharing 专项同时验证：

```text
baseline:  reg_count=4, reg_bits=32, delay_line_count=0
optimized: reg_count=2, reg_bits=16, delay_line_count=1, depth_total=2
cycles=257 checksum=16967771760616252376
```

Gate 记录见
[run_examples summary](gates/logs/delay-line-combine-20260812-final3/summary.json) 和
[run_sims summary](gates/logs/delay-line-combine-20260812-final3/run_sims_summary.json)。

从仓库根目录运行主要验证：

```bash
# 生成 before/after MLIR 和后端产物
PYTHONPATH=compiler/frontend python3 \
  verification/delay_line_combine/generate_artifacts.py --benchmark-depth 128

# C++ primitive 与 Verilator/生成模型等价
c++ -std=c++17 -O2 -Iruntime \
  verification/delay_line_combine/runtime_delay_line_test.cpp \
  -o /tmp/runtime_delay_line_test
/tmp/runtime_delay_line_test
python3 verification/delay_line_combine/check_verilator_trace.py
python3 verification/delay_line_combine/check_generated_models.py

# 相同 delay-line 的结构共享与四模型动态等价
python3 verification/delay_line_combine/check_duplicate_shared_models.py

# C++ 性能与 perf stat
python3 verification/delay_line_combine/run_benchmark.py \
  --depth 128 --cycles 2000000 --repeats 5 --perf
```

## 12. 当前限制与后续工作

### 12.1 Yosys 综合结果

当前环境没有 Yosys，尚无 FF/mux/LUT/logic-cell 的 `stat -json` 数据。已有证据是
MLIR 逻辑状态统计、结构和仿真等价。单链状态位应保持、duplicate 状态应减少，
但实际运行统一 Yosys 实验之前，不能将生成 Verilog 的文本缩减当成面积缩减。

### 12.2 Verilator 性能

Verilator 当前用于正确性；第 10 节 wall-clock/perf 只针对生成 C++ model。公平的
Verilator 性能矩阵还需统一 cycle、输入/checksum、编译 flags、CPU 环境，并分开
报告 build time 和 simulation runtime。

### 12.3 X-state

Sharing 动态验证覆盖启动 reset 和中途 reset。reset 前 Verilog 状态允许为 X；
C++/Verilog 完整四值一致性属于 pyc4.0 value-model hardening 总体工作。相关约束
见 [`pyc4.0-decisions.md`](rfcs/pyc4.0-decisions.md) 的 Decision 0061、0115、0116。

### 12.4 更一般的 delay 网络

当前只合并 frontend-owned、串行、控制完全相同且仅末端可观测的链。若需要多个
中间 tap，应设计 first-class multi-tap delay 或拆成多个最大安全段，而不是放松
one-use 证明。只有在 O(M²) sharing 比较成为可测编译热点后，才需要 DenseMap key。

## 13. 当前状态

截至 2026-08-13：Dialect/verifier、Frontend marker、合并与共享 Pass、C++
scalar/vector ring、Verilog primitive、分析/统计/probe 适配及专项验证均已实现；
pycc 默认启用该 Pass。C++ 长链收益已经实测，Yosys cell 数据和同规格 Verilator
性能仍待补充。

该优化没有跳过硬件周期。它把固定延迟历史从大量细粒度状态节点提升为具有明确
depth 的 MLIR 状态抽象：MLIR Pass 证明改写安全，C++ 后端高效保存同一历史，
Verilog 后端保留真实可综合时序。
