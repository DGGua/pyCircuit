# pyCircuit C++ Model 与 Verilator 性能对比实验

> 实验日期：2026-08-11。比较对象是同一份 PYC MLIR 分别经 `CppEmitter` 与 `VerilogEmitter → Verilator` 得到的单线程、无 trace cycle simulator。本文比较的是宿主机仿真效率，不是综合面积或硬件时钟频率。

> 可视化版本：[交互式 HTML 报告](08_cpp_vs_verilator_performance.html)。

## 1. 一页结论

本次重新生成并测试了 examples 中的 30 个硬件模型：

- 30/30 组最新 Verilog 成功生成并通过 Verilator 构建；
- 30/30 组 C++ model 成功生成并通过 GCC 构建；
- 两后端使用相同输入序列，4,096 周期内每周期采样全部输出，30/30 checksum 一致；
- 正式吞吐测试固定 CPU 0、单线程、关闭 trace，统一使用 GCC 13.1 `-O2 -DNDEBUG`，每项交替运行三次并取中位数。
- 全部 30 个 case、两个后端已补充无插桩 `perf stat`；Counter、BF16 FMAC、SW5809S 另有 `perf record`。核心事件与 cache 事件分组采集且 coverage 为 100%。

结果没有“Verilator 永远更快”或“pycc C++ 永远更快”这样的单一结论：

| 汇总 | 结果 |
|---|---:|
| pycc C++ model 更快 | 23 / 30 |
| Verilator 更快 | 7 / 30 |
| 中位样例 `Verilator/C++` | 0.270 |
| 30 项几何平均 `Verilator/C++` | 0.442 |
| Verilator 最大优势 | `dodgeball_game` top，4.01× |
| C++ model 最大优势 | `arith`，7.80× |

这里的“几何平均”只是 30 个大小不同样例的等权汇总，不能代表某个真实项目的总体加速比。更重要的规律是：

```text
很小/很浅的模型
    pycc C++ model 的轻量直接调用更快
        ↓ 设计复杂度增加
Verilator 的固定 trigger/settle 成本被摊薄
        ↓ 大量组合逻辑、复杂状态或多 FIFO
Verilator 的全局折叠、内联和紧凑状态更新开始明显占优
```

三个最有代表性的结果是：

| 模型 | C++ model | Verilator | 结论 |
|---|---:|---:|---|
| Counter | 45.95 M cycle/s | 10.14 M cycle/s | C++ model 快 4.53×，固定调度成本主导 |
| BF16 FMAC | 0.698 M cycle/s | 2.224 M cycle/s | Verilator 快 3.19×，大组合网表全局优化占优 |
| SW5809S | 1.407 M cycle/s | 4.491 M cycle/s | Verilator 快 3.19×，组合求值和 FIFO 状态提交共同成为 C++ 热点 |

## 2. 产物刷新与实验范围

### 2.1 如何保证 `.v` 是最新的

首先对当前 MLIR 编译器做了增量 Release 构建与安装：

```bash
cmake --build .pycircuit_out/toolchain/build-local-llvm19-zstd -j 8
cmake --install .pycircuit_out/toolchain/build-local-llvm19-zstd
```

本机 staged `pycc` 在**代码生成阶段**还需要通过 `LD_LIBRARY_PATH` 指向本地 LLVM 19 动态库（`/tmp/pyc-llvm19-local-20260810/root/usr/lib/llvm-19/lib` 与相邻的 `x86_64-linux-gnu` 目录）。该变量没有传给后续 benchmark 可执行文件，因此不会改变两种模型的运行时环境或性能结果。

然后确认 30 份 examples `.pyc` 均晚于对应 example/frontend 源文件。每个 case 从**同一份** `.pyc` 分别生成两后端，参数一致：

```bash
pycc case.pyc --emit=verilog \
  --build-profile=release \
  --inline-policy=off \
  --hierarchy-policy=strict \
  --logic-depth=256 \
  --out-dir <case>/verilog

pycc case.pyc --emit=cpp --cpp-split=module \
  --build-profile=release \
  --inline-policy=off \
  --hierarchy-policy=strict \
  --logic-depth=256 \
  --out-dir <case>/cpp
```

最新生成物位于 [`.pycircuit_out/perf-cpp-vs-verilator-20260811`](../.pycircuit_out/perf-cpp-vs-verilator-20260811)。共生成 30 个 top `.v`、30 份 `pyc_primitives.v` 和 30 组 C++ manifest/model。

没有覆盖 `designs/examples/*/build/verilog` 中的历史产物，因为仓库当前的 artifact policy 要求新生成物放在 `.pycircuit_out`。

### 2.2 为什么不用现有短 testbench 直接计时

examples 中的 Python testbench 主要用于几到几十周期的功能回归。若直接测它们：

- 进程启动、模型构造和 Python/测试框架开销会超过 DUT 求值；
- 不同 testbench 的 expect、打印、trace 与结束条件不同；
- 部分特殊样例没有标准 `tb_<name>.py`，不能形成全量统一基线；
- 很短的运行不能稳定测出 cycle/s。

因此现有 testbench 用于理解功能和刺激意图；性能实验另外生成成对 native harness。生成器为 [`run_experiment.py`](benchmarks/cpp_vs_verilator/run_experiment.py)，每个 case 的实际 harness 保存在 `<case>/bench/bench_cpp.cpp` 与 `bench_verilator.cpp`。

## 3. 公平性设计

### 3.1 完全相同的条件

| 项目 | C++ model | Verilator |
|---|---|---|
| MLIR 输入 | 同一份 `.pyc` | 同一份 `.pyc` |
| C++ 编译器 | g++ 13.1 | g++ 13.1 |
| 优化 | `-O2 -DNDEBUG` | `OPT_FAST/SLOW/GLOBAL=-O2`、`-DNDEBUG` |
| 主机/CPU | 同一主机，固定 CPU 0 | 同一主机，固定 CPU 0 |
| 线程 | 1 | `--threads 1` |
| trace | off | off |
| 输入 | 同一确定性 64-bit 序列，按端口位宽 mask | 同左 |
| reset | 所有 `!pyc.reset` assert 2 cycle，再 deassert | 同左 |
| 重复次数 | 3，取中位数 | 3，取中位数 |

Verilator 自身必须带一些生成器/runtime 编译选项，无法做到命令行字节级完全一致；本实验保证的是核心优化等级、编译器、线程、CPU、刺激和观察行为一致。

### 3.2 一个逻辑周期如何执行

C++ model：

```cpp
dut.comb();
clk = 1;
dut.tick();
dut.transfer();
dut.comb();
clk = 0;
dut.tick();
dut.transfer();
```

Verilator：

```cpp
dut.eval();          // low phase，输入变化后的组合稳定
clk = 1;
dut.eval();          // posedge + NBA + settle
clk = 0;
dut.eval();          // negedge bookkeeping + settle
```

这不是“双方调用次数必须一样”，而是让双方经历相同的输入变化、上升沿、状态提交和下降沿。调用次数不同正是两个 simulator 的调度实现差异，也是待测成本的一部分。

多时钟样例中所有 `!pyc.clock` 在本实验中同相切换。这能保证两后端可比，但不代表该设计所有异步相位 workload。

### 3.3 如何防止测到错误结果或空循环

实验有两个观察强度：

1. **等价检查**：4,096 周期，每周期读取全部输出并滚动 checksum；30/30 相同。
2. **性能检查**：每 4,096 周期读取一次全部输出，减少观察代码对小 DUT 的污染；最终 checksum 被打印，避免计算被删除。

每个 case 先做 100,000 周期校准，再让较慢后端的正式单次运行接近 0.35 秒，双方使用相同 cycle 数。三次运行交替后端顺序，减轻温度和系统负载偏差。

## 4. 全部结果

下表 `V/C` 表示 `Verilator cycles/s ÷ C++ model cycles/s`：大于 1 是 Verilator 更快，小于 1 是 C++ model 更快。原始数值见 [`results_20260811.csv`](benchmarks/cpp_vs_verilator/results_20260811.csv) 和 [实验 JSON](../.pycircuit_out/perf-cpp-vs-verilator-20260811/results/results.json)。

| Case | C++ model cycle/s | Verilator cycle/s | V/C |
|---|---:|---:|---:|
| arith | 76.60 M | 9.82 M | 0.128 |
| boundary_value_ports | 75.23 M | 10.10 M | 0.134 |
| cache_params | 71.35 M | 9.88 M | 0.138 |
| calculator | 5.64 M | 6.83 M | **1.211** |
| counter | 45.95 M | 10.14 M | 0.221 |
| decode_rules | 44.96 M | 9.66 M | 0.215 |
| digital_clock | 3.88 M | 8.41 M | **2.169** |
| digital_filter | 8.78 M | 9.61 M | **1.094** |
| dodgeball_game / VGA | 10.46 M | 9.38 M | 0.897 |
| dodgeball_game / top | 1.76 M | 7.07 M | **4.010** |
| fastfwd | 27.82 M | 6.89 M | 0.248 |
| fifo_loopback | 30.46 M | 8.47 M | 0.278 |
| fm16 / npu_node | 4.34 M | 3.62 M | 0.834 |
| fm16 / sw5809s | 1.41 M | 4.49 M | **3.192** |
| fmac / bf16_fmac | 0.698 M | 2.224 M | **3.188** |
| hier_modules | 68.52 M | 10.15 M | 0.148 |
| issue_queue_2picker | 7.58 M | 7.20 M | 0.951 |
| jit_control_flow | 41.13 M | 8.69 M | 0.211 |
| jit_pipeline_vec | 11.93 M | 9.45 M | 0.792 |
| mem_rdw_olddata | 47.21 M | 9.09 M | 0.193 |
| multiclock_regs | 37.34 M | 9.77 M | 0.262 |
| net_resolution_depth_smoke | 57.51 M | 10.05 M | 0.175 |
| obs_points | 55.86 M | 9.18 M | 0.164 |
| pipeline_builder | 19.38 M | 9.75 M | 0.503 |
| reset_invalidate_order_smoke | 26.39 M | 8.42 M | 0.319 |
| struct_transform | 17.18 M | 9.54 M | 0.555 |
| sync_mem_init_zero | 46.97 M | 9.01 M | 0.192 |
| traffic_lights_ce | 3.57 M | 6.37 M | **1.787** |
| wire_ops | 49.36 M | 9.95 M | 0.202 |
| xz_value_model_smoke | 62.12 M | 10.10 M | 0.163 |

## 5. 结果应该怎样理解

### 5.1 小模型：Verilator 固定调度成本没有被摊薄

多个极小模型的 Verilator 吞吐集中在约 9–10 M cycle/s，而 C++ model 可达到 40–76 M cycle/s。这种近似“平台”很像固定成本，而不是 DUT 逻辑成本。

Counter 的独立 O2+gprof 归因中，Verilator 采样大致分布为：

| Verilator Counter 热点 | gprof self time |
|---|---:|
| `VlDeleter::deleteAll` | 28.8% |
| root `eval` | 21.9% |
| `Verilated::endOfEval` | 13.7% |
| message queue `flush` | 11.6% |
| `eval_step` | 8.2% |
| NBA phase | 4.1% |

即使 `--threads 1`，Verilator 仍保留统一的 trigger、message queue、end-of-eval 等调度框架。Counter 的真实逻辑只有一个 8-bit 加法、mux 和寄存器，这些固定成本比逻辑本身更大。

开启硬件计数器后的 `perf record` 给出了更具体的证据：Verilator Counter 中约 35.0% 的周期样本落在 `pthread_mutex_trylock`，约 25.7% 落在 `pthread_mutex_unlock`，调用链来自 `VlDeleter::deleteAll → eval_step`。它每个逻辑周期执行约 575 条指令和 102 个分支；C++ model 只有约 161 条指令和 30 个分支。也就是说，这里不是 C++ model 的 IPC 特别高，而是 Verilator 为同一个极小逻辑周期执行了约 3.57 倍指令。

C++ model 对 Counter 则是直接函数调用。其 `eval()` 只执行常量、加法、mux 和输出连接（[`counter.cpp`](../.pycircuit_out/perf-cpp-vs-verilator-20260811/counter/counter/cpp/counter.cpp#L67)），因此小设计明显占优。

这也解释了为什么 `mem_rdw_olddata` 或 `fifo_loopback` 的结果不能被直接解读成“C++ memory/FIFO 永远更快”：这些样例本身太小，首先测到的是 simulator 固定开销。

### 5.2 BF16 FMAC：全网表求值开始压垮 C++ model

BF16 FMAC 的生成 C++ 包含：

- 1,189 个持久化 Wire 字段；
- 31 个 `eval_comb`/分片函数；
- 约 1,182 条生成赋值；
- 每个逻辑周期两次 `comb/eval`。

因此一次周期大约触发两遍大型组合网表。gprof 中超过 75% 的采样落在 `eval_comb_19/20/2/22/18` 等组合块，热点报告见 [`bf16_fmac/cpp.txt`](../.pycircuit_out/perf-cpp-vs-verilator-20260811/results/gprof/bf16_fmac/cpp.txt)。生成入口 `eval()` 位于 [`bf16_fmac.cpp`](../.pycircuit_out/perf-cpp-vs-verilator-20260811/fmac/bf16_fmac/cpp/bf16_fmac.cpp#L1515)。

Verilator 的 gprof/perf 中：

- combinational ICO 内核约 54.4%；
- NBA sequential 内核约 20.2%；
- root eval 调度约 14.0%。

固定框架仍存在，但设计逻辑已经足够大，Verilator 能通过 RTL constant propagation、wire folding、表达式合并和生成 C++ 内联把它摊薄。pycc C++ Emitter 更接近“把 MLIR op 机械翻译成持久字段赋值”，优化器较难跨越如此多的对象 load/store 恢复成紧凑表达式。因此 Verilator 最终快 3.19×。

硬件计数进一步显示：C++ model 每逻辑周期执行约 8,719 条指令、1,743 次 L1D load，而 Verilator 分别约为 3,537 条和 836 次；前者工作量是后者的 2.47× 和 2.09×。同时 Verilator IPC 为 2.85，高于 C++ 的 2.25。`perf record` 的 C++ 前十大热点几乎全部是 `eval_comb_*`、`tick_compute` 与 `tick_commit`，与 gprof 的归因一致。

### 5.3 SW5809S：组合求值与状态提交同时变重

SW5809S 含 16 个 `pyc_fifo<32,4>`。C++ gprof 大致显示：

- `eval()` 自身与各 `eval_comb_*` 合计约六成；
- `tick_compute()` 约 18.6%；
- `tick_commit()` 约 18.9%。

报告见 [`sw5809s/cpp.txt`](../.pycircuit_out/perf-cpp-vs-verilator-20260811/results/gprof/sw5809s/cpp.txt)。top 的 `eval/tick` 入口见 [`sw5809s.cpp`](../.pycircuit_out/perf-cpp-vs-verilator-20260811/fm16/sw5809s/cpp/sw5809s.cpp#L455)。

这与 runtime FIFO 的实现相吻合：`tick_compute()` 每次 posedge 把整个 `storage_` 复制到 `storageNext_`（[`pyc_primitives.hpp`](../runtime/cpp/pyc_primitives.hpp#L253)），`tick_commit()` 再全部复制回去（[`pyc_primitives.hpp`](../runtime/cpp/pyc_primitives.hpp#L276)）。即使一个周期只写一个 entry，也有两遍 `O(Depth × Width)` copy。

Verilator 将 RTL array/NBA 更新编译成较紧凑的条件状态更新。在它的 SW5809S gprof 中，NBA 内核约 38.1%、组合 ICO 约 19.1%；最终吞吐是 C++ model 的 3.19×。

perf 的每周期计数也支持这一判断：C++ model 约执行 4,546 条指令、623 个分支和 1,568 次 L1D load；Verilator 约为 1,470 条、166 个和 474 次，分别少 3.09×、3.75× 和 3.31×。C++ 的 `perf record` 中 `eval` 约占 29.2%、`tick_compute` 约 23.4%、`tick_commit` 约 15.9%，表明组合逻辑和状态提交确实同时消耗 CPU。

### 5.4 `npu_node` 为什么没有跟着 SW5809S 一起反转

`npu_node` 也是 FIFO 模型，但 C++ model 仍快约 20%。原因是本次随机 workload、FIFO 数量/连接拓扑和组合逻辑规模与 SW5809S 不同；同时 Verilator 在 `npu_node` 上的绝对吞吐只有 3.62 M cycle/s，说明它也承担了更大的 RTL 求值工作。

因此不能只按“是否含 FIFO”分类，应至少同时观察：

- primitive 数量、Width、Depth；
- 每周期 push/pop 活跃率；
- 组合网表节点数和逻辑深度；
- cache hit ratio；
- `tick_compute/commit` 与 comb 的时间占比。

## 6. 全部 30 个 example 的 perf 分析

用户将本机 `kernel.perf_event_paranoid` 从 3 临时调整为 2 后，`perf 5.4.291` 已能采集用户态硬件事件。正式吞吐结果没有重跑或替换；perf 使用同一批未插桩 `-O2 -DNDEBUG` executable，固定 CPU 0，并以 `:u` 排除内核事件。

### 6.1 全量采集方法与质量门槛

30 个 case 的 C++/Verilator 后端全部执行 `perf stat`：

- 30 case × 2 backend × 2 event group，共 120 次 `perf stat` 调用；
- 每次 `perf stat -r 3`，实际执行 360 次 benchmark；
- core 组为 cycles、instructions、branches、branch-misses；
- cache 组为 L1D loads/misses、LLC loads/misses；
- 同一 case 的两个后端使用相同逻辑周期数；周期数按较快后端至少运行约 0.25 秒校准；
- case 间交替后端顺序，同时交替 core/cache 组顺序；
- core/cache 分组后，全部事件 enabled coverage 为 100%，没有 multiplexing；
- 若三次 core hardware cycles 的标准差超过 3%，脚本自动重采，最终最坏值为 2.69%。

可恢复的采集与解析脚本是 [`run_perf_all.py`](benchmarks/cpp_vs_verilator/run_perf_all.py)。60 行完整规格化数据见 [`perf_stat_all_20260811.csv`](benchmarks/cpp_vs_verilator/perf_stat_all_20260811.csv)，原始 stderr/stdout 与 JSON 位于 [`results/perf-stat-all`](../.pycircuit_out/perf-cpp-vs-verilator-20260811/results/perf-stat-all)。

### 6.2 30 个 case 的每逻辑周期工作量

下面 `C/V` 均为 C++ model 除以 Verilator。小于 1 表示 C++ 每个逻辑周期做的工作更少；大于 1 表示 Verilator 更少。为保持主表可读，分支、L1D、miss rate、IPC 和统计方差放在完整 CSV 中。

| 样例 | C++ 指令/周期 | Verilator 指令/周期 | 指令 C/V | HW cycle C/V |
|---|---:|---:|---:|---:|
| `arith/arith` | 76 | 605 | 0.126 | 0.129 |
| `boundary_value_ports/boundary_value_ports` | 83 | 596 | 0.139 | 0.136 |
| `cache_params/cache_params` | 73 | 601 | 0.122 | 0.140 |
| `calculator/calculator` | 1,022 | 1,107 | 0.923 | 1.208 |
| `counter/counter` | 161 | 575 | 0.280 | 0.220 |
| `decode_rules/decode_rules` | 155 | 628 | 0.247 | 0.209 |
| `digital_clock/digital_clock` | 1,207 | 778 | 1.551 | 1.997 |
| `digital_filter/digital_filter` | 605 | 621 | 0.975 | 1.099 |
| `dodgeball_game/lab_final_VGA` | 593 | 638 | 0.929 | 0.892 |
| `dodgeball_game/lab_final_top` | 3,187 | 982 | 3.244 | 3.949 |
| `fastfwd/fastfwd` | 201 | 787 | 0.255 | 0.242 |
| `fifo_loopback/fifo_loopback` | 262 | 730 | 0.359 | 0.282 |
| `fm16/npu_node` | 1,645 | 1,805 | 0.911 | 0.802 |
| `fm16/sw5809s` | 4,546 | 1,471 | 3.091 | 3.061 |
| `fmac/bf16_fmac` | 8,721 | 3,540 | 2.464 | 3.142 |
| `hier_modules/hier_modules` | 70 | 596 | 0.118 | 0.147 |
| `issue_queue_2picker/issue_queue_2picker` | 829 | 1,021 | 0.813 | 0.956 |
| `jit_control_flow/jit_control_flow` | 198 | 643 | 0.308 | 0.204 |
| `jit_pipeline_vec/jit_pipeline_vec` | 456 | 606 | 0.752 | 0.808 |
| `mem_rdw_olddata/mem_rdw_olddata` | 170 | 646 | 0.263 | 0.197 |
| `multiclock_regs/multiclock_regs` | 177 | 611 | 0.290 | 0.267 |
| `net_resolution_depth_smoke/net_resolution_depth_smoke` | 129 | 574 | 0.225 | 0.175 |
| `obs_points/obs_points` | 117 | 663 | 0.177 | 0.166 |
| `pipeline_builder/pipeline_builder` | 282 | 595 | 0.474 | 0.517 |
| `reset_invalidate_order_smoke/reset_invalidate_order_smoke` | 161 | 575 | 0.280 | 0.227 |
| `struct_transform/struct_transform` | 333 | 604 | 0.551 | 0.549 |
| `sync_mem_init_zero/sync_mem_init_zero` | 170 | 646 | 0.263 | 0.199 |
| `traffic_lights_ce_pyc/traffic_lights_ce` | 1,560 | 1,101 | 1.416 | 1.872 |
| `wire_ops/wire_ops` | 165 | 589 | 0.281 | 0.203 |
| `xz_value_model_smoke/xz_value_model_smoke` | 103 | 573 | 0.180 | 0.163 |

### 6.3 全量 perf 改变或加强了哪些判断

1. **硬件周期能完整解释本次吞吐胜负。** 30/30 case 中，HW cycle 更少的一方就是正式吞吐更快的一方；两种比值的 log Pearson correlation 为 0.998。它不是新的独立性能指标，而是证明吞吐结果确实来自 CPU 求值工作量，而非 I/O 或计时器异常。
2. **动态指令数解释 28/30 个胜负。** C++ model 在 25/30 case 中执行更少指令，而吞吐胜出 23 个。两个例外是 `calculator` 和 `digital_filter`：C++ 指令略少 7.7% 和 2.5%，但 IPC 只有 2.09/1.93，而 Verilator 为 2.73/2.17，所以 Verilator 仍快 1.21×/1.09×。
3. **小模型的 Verilator 固定成本非常一致。** `arith`、`boundary`、`cache_params`、`hier_modules` 每周期的 Verilator 指令都约 596–605 条；对应 C++ 只有约 70–83 条。这印证了约 9–10 M cycle/s 的吞吐平台来自统一 runtime 路径。
4. **复杂模型的反转来自工作量真正反转。** `dodgeball top`、`SW5809S`、`BF16 FMAC` 的 C++ 指令分别是 Verilator 的 3.24×、3.09×、2.46×；不只是相同指令跑得慢。
5. **cache miss 不是当前主因。** 60 个后端结果中最大 L1D miss rate 只有 0.0169%，最大 branch miss rate 为 0.933%。LLC load 数很低且容易受进程启动影响，不应根据 LLC miss 百分比单独下结论。更稳定且直接的信号是每周期 instructions、branches 与 L1D loads。

30 个 case 的等权几何平均 `instructions C/V` 为 0.449，表示从样例等权角度，Verilator 平均执行约 2.23× 指令；这与 C++ 在多数小样例胜出一致。但该平均不代表真实项目权重，也不会否定复杂 case 中 C++ 工作量已经反转。

### 6.4 perf record 与 gprof 的函数级归因

全量使用 `perf stat`；函数级 `perf record -F 999 -e cycles:u` 继续选 Counter、BF16 FMAC、SW5809S 三个结构代表，且没有丢样本。Counter 使用 DWARF call graph；BF16/SW5809S 使用 flat symbol sampling，避免当前二进制中的压缩 DWARF 与旧版 `addr2line` 不兼容。采样数据位于 [`results/perf-record`](../.pycircuit_out/perf-cpp-vs-verilator-20260811/results/perf-record)。

perf record 与 gprof 的函数分类一致：Counter 的 Verilator 固定 runtime/锁路径主导；BF16 C++ 的 `eval_comb_*` 主导；SW5809S C++ 的 `eval` 与 `tick_compute/commit` 同时占大头。gprof 是第二种独立归因手段，但其 `-pg` 构建不参与吞吐或硬件计数。

三组 `/usr/bin/time` 中 system time 均约为 0，最大 RSS 都在约 3.8–4.3 MiB。综合 perf、gprof 与 time，trace-off 下的瓶颈是 CPU 求值工作量，不是文件 I/O 或内存容量。

## 7. 对 pyCircuit 优化工作的含义

### 7.1 不要用 Counter 作为唯一性能代表

只测 Counter 会得出“C++ model 比 Verilator 快 4.5×”；只测 BF16/SW5809S 又会得出“Verilator 快 3.2×”。两者都是真实结果，但只覆盖了复杂度曲线的一端。

性能门禁至少应保留四类：

1. 小型纯组合：`arith`；
2. 小型时序：`counter`；
3. 大型 datapath：`bf16_fmac`；
4. 多 primitive/state：`sw5809s` 或 `npu_node`。

### 7.2 C++ Emitter 的优先优化点

1. **减少持久化组合 SSA**：端口、状态、primitive 绑定和 probe 保留为成员；不可观察的中间量尽量生成局部变量。
2. **dirty-region/event-driven eval**：利用 MLIR CombDepGraph，只重算受变化输入/状态影响的区域。这直接对应 Decisions 0103–0109。
3. **降低每周期两次全 eval 的成本**：保留 TICK-OBS/XFER-OBS 语义，但只更新变脏 region，不能简单删除 observation phase。
4. **FIFO 增量提交**：记录一次 push transaction 与 pointer/count next-state，不复制整个数组。
5. **边沿专用 API**：让生成 top 实现 `tick_posedge/tick_negedge`，避免下降沿遍历通用 tick/transfer 和无变化 cache invalidation。
6. **基于收益的 cache**：记录 primitive cache hit ratio；活跃输入下零命中的缓存不应携带一整组 version/fingerprint 成本。
7. **可选 LTO/性能 profile**：当前已经是 O2；LTO、O3、`-march=native` 应作为独立 A/B，不能把它们替代结构优化。

### 7.3 一个务实的后端选择策略

在当前版本中可以采用：

- 需要极快编译/启动、小模型迭代、轻量嵌入：优先 pycc C++ model；
- 大型 datapath、多状态、多 FIFO、长时间回归：同时生成 Verilator，先用 benchmark 决定；
- trace/DFX 或跨后端语义验证：两者都保留，按相同 observation point 比较；
- 不按生成代码行数猜性能，直接记录 cycles/s、eval/cycle、cache hit 和 primitive 状态成本。

## 8. 限制与不能过度解读的地方

1. 输入是确定性高活跃 synthetic stimulus，不代表应用真实概率分布；低活动 workload 可能更利于缓存/event-driven 模型。
2. 正式性能循环每 4,096 周期采样一次输出；trace、assert 和完整逐周期观测未计入吞吐。
3. X/Z trace、VCD、binary trace 与多线程都未测；`xz_value_model_smoke` 在这里仅比较普通无 trace 执行。
4. 多时钟统一同相切换，没有覆盖任意 clock ratio/phase。
5. 30 个 example 大小差异很大；“23 胜 7”不是按真实仿真时间加权的产品结论。
6. Verilator 是把 `.v` 编译为优化 C++ 后运行，不是解释执行 Verilog；本实验比较的是两条代码生成路线。
7. checksum 等价门禁覆盖 4,096 周期和全部端口，但不等于形式证明。任何调度语义改动仍需仓库 G2 跨后端回归。
8. perf 数值与本机微架构、编译器和 Verilator 版本相关；适合解释本次差距，不能直接外推到其他 CPU。

## 9. 复现实验

已有刷新产物时：

```bash
python3 reports/benchmarks/cpp_vs_verilator/run_experiment.py \
  --run-root .pycircuit_out/perf-cpp-vs-verilator-20260811 \
  --source-root .pycircuit_out/examples-cpp-models \
  --jobs 6 \
  --repetitions 3 \
  --target-seconds 0.35
```

只重跑已构建 executable：

```bash
python3 reports/benchmarks/cpp_vs_verilator/run_experiment.py \
  --run-root .pycircuit_out/perf-cpp-vs-verilator-20260811 \
  --source-root .pycircuit_out/examples-cpp-models \
  --skip-build \
  --repetitions 3 \
  --target-seconds 0.35
```

全量重跑或断点续跑 perf stat（要求 `kernel.perf_event_paranoid <= 2`）：

```bash
python3 reports/benchmarks/cpp_vs_verilator/run_perf_all.py \
  --run-root .pycircuit_out/perf-cpp-vs-verilator-20260811 \
  --cpu 0 \
  --repetitions 3 \
  --target-seconds 0.25 \
  --publish-csv reports/benchmarks/cpp_vs_verilator/perf_stat_all_20260811.csv
```

脚本会校验缓存的周期数和重复次数，只重采缺失或参数不匹配的结果；hardware cycles 三次标准差超过 3% 时最多自动重试三次。使用 `--force` 可明确要求全部重采。

脚本会持续更新：

- `.pycircuit_out/perf-cpp-vs-verilator-20260811/results/results.json`
- `.pycircuit_out/perf-cpp-vs-verilator-20260811/results/results.csv`
- 每个 case 的生成 harness 与构建日志。
- `.pycircuit_out/perf-cpp-vs-verilator-20260811/results/perf-stat-all/results.{json,csv}`
- `.pycircuit_out/perf-cpp-vs-verilator-20260811/results/perf-stat-all/raw/*.{perf.csv,stdout.txt}`

## 10. 最终判断

目前 pycc C++ model 的优势是调度轻、直接、对小设计非常快；劣势是大设计中仍接近“把全部 MLIR op 物化成字段并逐周期整网表重算”。Verilator 的优势是成熟的全局 RTL 优化与紧凑求值；劣势是每次 `eval()` 都要经过统一 trigger/settle/runtime 框架，小模型时固定成本很高。

因此优化目标不应是“让 C++ model 在 Counter 上继续赢更多”，而应是把 crossover point 向更大的设计移动：保持当前轻量调度优势，同时通过 ephemeral SSA、dirty-region、边沿专用调度和增量 FIFO state 把 BF16/SW5809S 一类模型的 3–4× 差距收回来。所有这些改动必须先在 MLIR 中定义依赖与状态语义，并通过相同刺激、相同 TICK/XFER 观察点的 C++/Verilator 等价门禁。
