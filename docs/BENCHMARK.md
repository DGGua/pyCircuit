# C++ 仿真与 Verilator Benchmark

`pycircuit benchmark` 用同一次 Python/JIT elaboration 得到的设计，比较
pyCircuit 生成的 C++ functional simulator 和 Verilator C++ model 的运行性能。
它不是普通的 `@testbench` 回归：工具会生成一个确定性的 native workload
harness，直接通过 DUT 的顶层端口驱动输入、采样输出并计算 digest。

在开始计时前，工具先做精确的跨后端 preflight；因此性能数字只在已验证的
同一 workload 上产生。

这落实了 Decision 0112 的“MLIR 是唯一语义来源，C++ 与 Verilog 必须逻辑
等价”约束，并按 Decision 0113 的观测点规则比较后端结果。它是性能工具，
不能替代 `run_sims.sh` 的完整功能/G2 gate。

## 准备

先准备 `pycc`、CMake/Ninja、C++ 编译器和 Verilator。开发树中推荐：

```bash
export PYC_TOOLCHAIN_ROOT="$PWD/.pycircuit_out/toolchain/install"
export PYCC="$PYC_TOOLCHAIN_ROOT/bin/pycc"
export PATH="$PYC_TOOLCHAIN_ROOT/bin:$PATH"

"$PYCC" --version
verilator --version
```

输入文件必须是 `pycircuit build` 可构建的 Python project entry：其中既有
`@module def build(...)`，也有装饰过的 `@testbench def tb(...)`。`tb` 仍是
frontend build contract 的一部分；benchmark 本身不计时该 TB schedule，而是
使用生成的 top-level harness。

## 两种入口

安装/开发环境都可使用 CLI 子命令：

```bash
python3 -m pycircuit.cli benchmark \
  designs/examples/counter/tb_counter.py \
  --mode clocked \
  --iterations 1000000 \
  --warmup-iterations 10000 \
  --verify-iterations 256 \
  --repeats 5 \
  --out-dir /tmp/pyc-counter-bench
```

仓库内也提供无需事先安装 frontend package 的 wrapper；参数与上面的
`pycircuit benchmark` 完全相同：

```bash
python3 flows/tools/perf/run_cpp_vs_verilator.py \
  designs/examples/counter/tb_counter.py \
  --mode clocked --iterations 1000000 --repeats 5
```

默认输出目录为
`.pycircuit_out/perf/cpp-vs-verilator/<source-stem>/`，默认 JSON 文件为
`<out-dir>/result.json`。用 `--output result.json` 可把结果写到指定位置。

## Workload 和观测语义

每个 native harness 使用相同的 seed 生成固定的 input table，并在每次迭代
驱动所有**数据输入**（所有不是 clock/reset 的 top-level 输入）。数据输入会在
每次迭代改变；input table 以 `i & (input_table_size - 1)` 循环复用，因此过程
可复现而不是 host RNG 测量。clock/reset 由 harness 控制，输出顶层端口被按字
采样并混入 digest。

`--mode auto` 在 top-level 有一个 `!pyc.clock` 输入时选择 `clocked`，否则选
`comb`。也可显式指定：

| 模式 | 每次迭代的工作 | 比较观测点 |
|---|---|---|
| `comb` | drive inputs 后执行 C++ `comb()` 或 Verilator `eval()` | settled combinational output |
| `clocked` | drive inputs，完成一个高电平边沿和低电平收尾 | C++ 的 `tick()`/`transfer()` 后、Verilator 对应正边沿 `eval()` 后的 XFER-OBS |

clocked C++ 路径在正边沿前先 `comb()`，然后执行 `tick()`、`transfer()` 和一次
`comb()`；Verilator 路径使用与之对应的 `eval()` 序列。若有 reset，harness 会在
测量前执行 `--reset-cycles` 个 asserted cycle 与 `--reset-settle-cycles` 个
deasserted settle cycle。

## Exact preflight 与计时边界

1. 两个后端各自运行 `--verify-iterations` 次（默认 256）。在每次迭代，harness
   按输出端口/word 的固定顺序产生 transcript。
2. 工具要求 transcript 完全一致，且 verify digest 一致；任一差异都会终止，
   不产生“成功”的性能结果。
3. 每个计时样本是一个 fresh process。该进程先初始化/reset，并进行
   `--warmup-iterations` 次未计时 warmup；随后才开始 native `steady_clock`
   的计时区间。
4. 计时区间包含：public-port input assignment、comb/eval 或完整 clock cycle，
   以及按 `--sample-every` 的输出 digest 采样。它不包含 Python/JIT、MLIR emit、
   C++/Verilator 编译、DUT 初始化/reset、preflight、warmup 或 process 启动的
   外层 wall time。

exact transcript 只覆盖 preflight 的 `--verify-iterations`。timed run 会比较双方
按 `--sample-every` 采样的 digest 和最终 digest；若希望每个 timed iteration 都
进入 digest，可使用 `--sample-every 1`，代价是把更多校验开销计入性能数字。

build 时间和外层 wall time仍会写入 JSON，便于审计，但 `comparison` 使用每个
backend 的**内部 timed throughput median**。测量 repeat 的运行顺序会交替
C++/Verilator，以降低固定顺序偏差；每个 timed run 的 digest 和 final digest
也必须跨后端相同，并且同一后端所有 repeat 均须确定。

工具强制关闭 trace 和 pyCircuit 运行时统计：`PYC_SIM_STATS=0`、
`PYC_SIM_FAST=0`、`PYC_KONATA=0`，并清除 `PYC_TRACE_DIR` 和
`PYC_SIM_STATS_PATH`。Verilator 采用单线程；两侧均以 release-style `-O3`
构建，`--native` 才额外启用 `-march=native`。

## 常用参数

| 参数 | 默认值 | 说明 |
|---|---:|---|
| `--mode` | `auto` | `auto`、`comb` 或 `clocked`。`clocked` 要求恰好有一个 clock。 |
| `--iterations` | 1,000,000 | 每个 timed sample 的 cycle（clocked）或 evaluation（comb）数。 |
| `--warmup-iterations` | 10,000 | 每个 fresh process 内、计时前的 warmup 数。 |
| `--verify-iterations` | 256 | exact preflight 的逐迭代比较数。 |
| `--repeats` | 5 | 每个 backend 的 fresh-process timed samples。 |
| `--seed` | 固定常数 | input table 的确定性随机 seed，可写十进制或 `0x...`。 |
| `--input-table-size` | 4096 | 预生成随机输入帧数；必须是 2 的幂。 |
| `--sample-every` | 256 | timed workload 的 digest 间隔；必须是 2 的幂。 |
| `--reset-cycles` / `--reset-settle-cycles` | 2 / 1 | clocked reset 序列；不计入 timed interval。 |
| `--logic-depth` | 256 | 传给 `pycc` 的 MLIR logic-depth gate。 |
| `--comb-policy` | 无 | `legacy` 或 `gsim` 预设；显式低层参数必须与预设一致。 |
| `--comb-update` | 由预设决定 | `always`、`guarded` 或 `dirty`。 |
| `--comb-reg-update` | `poll` | `poll` 保留本地寄存器输入 snapshot；`commit` 在寄存器实际改值时唤醒直接 consumer。 |
| `--comb-partition` | `none` | `none` 或显式启用静态 SuperNode 划分。 |
| `--comb-partition-max-nodes` | 35 | 静态 SuperNode 的最大 operation 数。 |
| `--max-port-bits` | 4096 | 单个允许的 top-level `iN` port 最大宽度。 |
| `--jobs` | 逻辑 CPU 数 | 构建并行度。 |
| `--cpu` | 无 | Linux 上将 native 子进程绑到一个 logical CPU。 |
| `--native` | off | 两个 native harness 都使用 `-march=native`；跨机器结果不可直接比较。 |
| `--min-sample-seconds` | 0.1 | 任一内部 timed sample 低于该值时写 warning。 |
| `--timeout-seconds` | 300 | 每个 build/run stage 的 timeout。 |
| `--param name=value` | 无 | 重复传递 JIT 参数覆盖。 |
| `--verilator` / `--cmake` / `--cxx` | 自动发现 | 覆盖工具可执行文件。 |

`--iterations`、`--verify-iterations`、`--repeats`、`--input-table-size`、
`--sample-every`、`--jobs`、`--logic-depth` 和 `--max-port-bits` 必须大于零；
reset/warmup 可为零。

结果 JSON 的 `comb_policy` 同时记录 `update`、`reg_update`、`partition`
和 `partition_max_nodes`，因此 `dirty+poll` 与 `dirty+commit` 的 A/B 结果
不会被误归为同一种调度配置。

## JSON 结果与日志

成功结果包含：

- `case`：source、top、mode、操作单位（`cycles` 或 `evaluations`）和 IR hashes；
- `contract`：同一 elaboration、trace-off、单线程和 Decision IDs；
- `workload`：seed、iterations、warmup、preflight、repeat、input activity 和 reset；
- `correctness`：exact transcript 数量、SHA-256、verify digest 和限制说明；
- `build`：frontend/pycc/CMake/Verilator 各阶段时间（不混入 runtime 比较）；
- `runs.cpp` / `runs.verilator`：原始样本与 median/min/max/p25/p75/MAD 统计；
- `comparison`：`cpp_over_verilator_throughput`、较快 backend 与 speedup；
- `environment`、`artifacts`、`warnings`：工具版本、CPU/commit、二进制与日志路径。

每个 stage 的 command、stdout 和 stderr 位于 `<out-dir>/logs/`。这使结果可以复现，
并保留 preflight 或 native build 失败的诊断。

## v1 限制和解读

- 仅支持 flat top-level `iN` 输入/输出；输入额外允许 `!pyc.clock` 与
  `!pyc.reset`。Bundle、vector、mem handle 或其他未 lower 的类型会被拒绝。
- 最多一个 clock、最多一个 reset；`--mode=clocked` 必须有一个 clock。多时钟/
  CDC workload 不属于 v1 比较范围。
- preflight 是 **2-state top-level** port-word 比较。Verilator 以 X initial/
  assignment 归零运行，且其 public-port ABI 不暴露 X/Z masks；因此这不是
  4-value/X/Z 等价证明。内部状态仅通过其对顶层输出的影响被覆盖。
- 所有 data inputs 都会随机化。该工具测量的是“持续活动”的 public-port
  workload；它不代表 idle、特定协议流量、真实软件程序或完整 TB schedule 的
  性能。
- exact transcript 是计时前检查，不是整个 timed workload 的逐周期等价证明；
  timed run 默认只按 `--sample-every` 及最终输出比较 digest。
- input table 在 native heap 上分配；估算大小超过 256 MiB 时工具会拒绝运行，
  可降低 `--input-table-size`。
- `--out-dir` 不能含空白字符；Verilator 5.x 生成的 Makefile 无法可靠保留
  `--Mdir` 和 user harness 路径中的空白。工程路径含空白时请显式指定无空白的
  `--out-dir`（例如 `/tmp/pyc-bench`）。
- v1 native build 使用 GCC/Clang 风格的 `-O3`、`-fno-lto`（可选
  `-march=native`）参数；MSVC/clang-cl 尚未纳入支持范围。
- 短样本会被标记 warning。应提高 `--iterations`，使用多次 `--repeats`，在固定
  机器/CPU affinity 下比较 median 和分位数；不要把一次很短的运行或跨机器的
  `--native` 结果当作回归结论。

对于完整后端功能等价，仍应运行 G2：

```bash
PYC_GATE_RUN_ID=local-benchmark-preflight bash flows/scripts/run_sims.sh
```
