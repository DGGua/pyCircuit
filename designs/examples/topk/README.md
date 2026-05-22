# Top-K 流式计算模块（pyCircuit · 统一硬件版）

`designs/examples/topk/` 实现一个面向 GEMM/Attention 后置 Top-K 选取的**单一统一**
硬件模块。一次 `compile_cycle_aware(topk.build, P=256, K_MAX=4096, idx_w=12)` 出
**一个**电路，运行时同时支持：

- `fmt_sel` ∈ {bf16, fp16, fp32}：**运行时**输入，2 bit
- `k_in` ∈ [1, K_MAX]：**运行时**输入，模块自己算 `rows_used = ceil(k_in / P)`
- **零握手启动**：复位后 `ready_out=1` 立刻可用（SRAM 通过每行 `init_done` bit 透明初始化）

算法骨架：每周期吞进 `P` 个 (val, idx) 对，Stage A 做一个全宽 `P`-element Bitonic
sort，Stage B 走单一 always-streaming 路径，把 chunk 排序结果与 K_MAX/P 行 SRAM
里的当前 Top-K 滚动合并。

- **算法已端到端验证**（`tb_topk.py`）：3 fmt × K ∈ {1, 7, 8, 256, 257, 1024, 4096}
  × N 到 1M 全部对 numpy golden（`fp_to_unsigned_key`-排序）匹配，含
  NaN/Inf/denormal/全等/严格升降序边界。
- **RTL 已通过编译**：单次 `compile_cycle_aware(P=256, K_MAX=4096, idx_w=12)`
  emit 出 ~41 MB MLIR，Stage A 4608 cmp_swap、Stage B merge 2304 cmp_swap。

## 1. 顶层模块

`topk.build()` 是唯一对外的 `compile_cycle_aware` 入口。

### 1.1 顶层端口

下面是 `topk.build()` 在 RTL 边界上暴露的所有端口。编译期参数（`P`, `K_MAX`,
`idx_w`）决定每个端口的实际位宽；除此之外所有维度（fmt / K / N）都在运行时由
端口决定。

**输入 — 数据流（每周期吞 1 个 chunk）**

| 端口 | 宽度 | 功能 |
| --- | --- | --- |
| `chunk_vals`  | `P × VAL_W` (`P × 32`) | 当前 chunk 的 P 个 val，lane 0 在 LSB。bf16/fp16 用每个 32-bit lane 的低 16 位（高 16 位 don't-care） |
| `chunk_idxs`  | `P × idx_w`            | 当前 chunk 的 P 个 idx（一般是全局元素下标），与 `chunk_vals` 同 lane 排列 |
| `valid_in`    | 1                       | 当前 chunk 是否有效。仅当 `ready_out=1 & valid_in=1` 才被吃进去 |

**输入 — 运行时配置（session 期内保持稳定）**

| 端口 | 宽度 | 功能 |
| --- | --- | --- |
| `fmt_sel`     | 2                       | 0=bf16, 1=fp16, 2=fp32。在所有 `cmp_swap` 内部驱动 `fp_lt` 的 3 路 monotone-key 选择 |
| `k_in`        | `ceil(log2(K_MAX+1))`   | 本 session 想要的 K ∈ [1, K_MAX]。模块据此算 `rows_used = ceil(k_in / P)`，决定 Stage B 在 MERGE_LOOP 里跑几行 |

**输入 — 读取接口**

| 端口 | 宽度 | 功能 |
| --- | --- | --- |
| `topk_sb_drain_addr` | `log2(K_MAX/P)` | 选哪一行 SRAM 暴露到 `topk_vals/topk_idxs`。IDLE 期间驱动；row 0 = 全局 Top-P |

**输出**

| 端口 | 宽度 | 功能 |
| --- | --- | --- |
| `topk_vals`     | `P × VAL_W` | `drain_addr` 指定那行的 P 个 val，descending 排序 |
| `topk_idxs`     | `P × idx_w` | 对应 idx |
| `running_valid` | 1           | 至少 1 个 chunk 完整 absorb 之后 sticky 为 1。复位后为 0 |
| `ready_out`     | 1           | 顶层 issue throttle 输出；1 = 这一拍可拉 `valid_in`，0 = 上游必须等 |

**协议要点**

- **零握手启动**：复位后 `ready_out` 立刻为 1。SRAM 的每行 `init_done` bit 复位归零，未写过的行读出来是 `neg_inf(fmt_sel)` 整行，所以第一个 chunk 不需要任何 sweep 配置。
- **背压**：`ready_out` 在 MERGE_LOOP 期间和 issue throttle 倒计时期间为 0；上游必须在 `ready_out=0` 的周期把 `valid_in` 拉低。两次 issue 之间至少 `rows_used + 1` cy。
- **稳定性约束**：一个 session 内 `fmt_sel` 与 `k_in` 必须保持稳定；中途切换会污染 running SRAM（硬件不会卡死，但结果无意义），上层须先 reset 再换。
- **drain**：要读完整 Top-K 需要在 IDLE 期间按拍轮流驱动 `drain_addr = 0..rows_used-1`（每拍读一行 P 个元素）。如果只要 Top-P，固定 `drain_addr=0` 即可。

### 1.2 build 树

下面是 `topk.build()` 实例化出来的 pyCircuit 子模块 / cell 树，**没有任何编译期
K-vs-P 分支**——所有 K 值共用同一棵子树，所有 fmt 共用同一个 `cmp_swap`。

```
topk.build(P, K_MAX, idx_w)                       ← compile_cycle_aware 入口
│
├── topk_config                                   [compile-time only]
│   ├── FMT_BF16=0 / FMT_FP16=1 / FMT_FP32=2     ← 2-bit fmt_sel 编码
│   ├── VAL_W = 32                                ← 统一 lane val 宽度
│   └── k_in_w(K_MAX), rows_used_w(P, K_MAX) …    ← 端口位宽帮手
│
├── Issue throttle (top-level)
│   └── 1 个 `issue_cnt` reg：保证两次 valid_in 间隔 ≥ rows_used+1 cy
│
├── stage_a.stage_a()                             ── 每 cy 吃 P 个 → P 个 desc-sorted
│   └── local_sort.bitonic_sort_desc(W=P)
│       ├── bitonic_schedule.gen_sort_schedule_desc(P)         [pure Python]
│       └── cmp_swap.cmp_swap_const_dir   × {P/2 × log₂P · (log₂P+1) / 2}
│           └── fp_compare.fp_lt(a32, b32, fmt_sel)
│               └── 3 路并联 monotone-key 路径 + fmt_sel mux
│
├── stage_b.stage_b()                             ── always-streaming
│   ├── FSM regs        :  state, rcnt(addr_w), vseen
│   ├── init_done reg   :  K_MAX/P bits           ← 透明初始化关键
│   ├── carry reg       :  P × (VAL_W + idx_w)
│   ├── running SRAM    :  m.sync_mem(depth=K_MAX/P, width=P·(VAL_W+idx_w))
│   ├── read mux        :  id[r]? rdata : neg_inf_row(fmt_sel)
│   └── merge_cell.bitonic_merge_2p_full(P)       ── 1 cy 组合段
│       ├── bitonic_schedule.gen_full_merge_2p_desc(P)         [pure Python]
│       └── cmp_swap.cmp_swap_const_dir × {P · (log₂(2P))}
│           └── fp_compare.fp_lt
│
└── 输出 packing :
    topk_vals[P]   topk_idxs[P]   running_valid   ready_out
```

几点说明：

- 方括号 `[…]` 标注的节点是「编译期产物」，不消耗任何硬件资源。
- 改 fmt 不再换子树——`fp_lt` 内部 3 路 monotone-key 并联，**运行时**由
  `fmt_sel` 二位 mux 选择，每个 `cmp_swap` 只有一个 comparator 网络。
- 改 K 不再换路径——`stage_b` 永远走 streaming，`rows_used` 由 `k_in` 在
  运行时算出来；K ≤ P 时 `rows_used = 1`，硬件吞吐自然降到 2 cy/chunk。

## 2. 架构总览

```
                   chunk_vals[P*32]   chunk_idxs[P*idx_w]
                   valid_in   fmt_sel[2]   k_in   drain_addr
                              │
                              ▼
       ┌────────────────────────────────────────────────────────┐
       │ Issue throttle                                          │
       │   rows_used = ceil(k_in / P)                            │
       │   gate valid_in so spacing >= rows_used+1 cy            │
       └─────────────────────────┬──────────────────────────────┘
                                 │
                                 ▼
       ┌────────────────────────────────────────────────────────┐
       │ Stage A : full P-element bitonic sort (W = P)          │
       │   fully pipelined                                      │
       │   log2(P) · (log2(P)+1) / 2 layers × P/2 cmp_swap      │
       │   P=256 → 36 layers × 128 = 4608 cmp_swap              │
       └─────────────────────────┬──────────────────────────────┘
                                 │ P lanes desc-sorted per cycle
                                 ▼
       ┌────────────────────────────────────────────────────────┐
       │ Stage B : SRAM streaming running merge (unified path)  │
       │   FSM: IDLE → MERGE_LOOP[rows_used cy] → IDLE          │
       │   bitonic_merge_2p_full(P)                             │
       │     log2(2P) layers × P cmp_swap                       │
       │     P=256 → 9 × 256 = 2304 cmp_swap                    │
       │   carry reg (P lanes)                                  │
       │   running SRAM (K_MAX/P rows × P·(VAL_W+idx_w))        │
       │   init_done[K_MAX/P]：未写过的行 → neg_inf(fmt_sel)    │
       │   ready=0 在 MERGE_LOOP 期间                           │
       └─────────────────────────┬──────────────────────────────┘
                                 │
                                 ▼
                   topk_vals[P]   topk_idxs[P]
                   running_valid  ready_out
```

详细原理图见 `figures/topk_all.png` （以及单图）。

## 3. 文件结构

| 文件 | 作用 |
| --- | --- |
| `topk_config.py`      | 编译期常量 (`P`, `K_MAX`, `IDX_W`, `VAL_W=32`)、运行时 fmt 编码、`FpFormat` 表、端口位宽帮手 |
| `bitonic_schedule.py` | 纯 Python 调度生成器（`gen_sort_schedule_desc`, `gen_full_merge_2p_desc`）。带软件模型 |
| `fp_compare.py`       | `fp_lt(a32, b32, fmt_sel)`：3 路并联 monotone-key + fmt_sel mux；NaN→-∞ 折叠（硬件） |
| `tool.py`             | Python 软参考：`fp_to_unsigned_key`、`fp_lt_py`、`float_to_bits` / `bits_to_float` |
| `selftest/`           | 纯 Python 自检（`test_all.py` 一次跑齐） |
| `cmp_swap.py`         | (val, idx) CMP-SWAP cell：`fmt_sel` 端口、动态 `dir` 与编译期 `direction` 两个版本 |
| `local_sort.py`       | `bitonic_sort_desc(W)`：按 schedule 装配，可选层间寄存器 |
| `merge_cell.py`       | `bitonic_merge_2p_full(P)`：2P → 2P 全量 merge，valley-bitonic 接线 |
| `stage_a.py`          | Stage A：全 P-sort，全流水（`log2(P)·(log2(P)+1)/2` 层） |
| `stage_b.py`          | Stage B 统一 streaming：FSM + `sync_mem` + `init_done` + carry reg + drain 接口 |
| `topk.py`             | 顶层：unify build；运行时 `fmt_sel`/`k_in` 输入；issue throttle |
| `tb_topk.py`          | numpy golden 验证（Python 软件模型 + 3 fmt × 7 K × 多 N 矩阵 + 边界） |
| `figures/draw_arch.py`| 原理图脚本，生成 `figures/*.png` |

## 4. 关键 cell 与层数

### 4.1 `cmp_swap` 真值表

```
swap = ~(dir XOR lt) = (dir == lt)         其中 lt = fp_lt(a, b, fmt_sel)
```

| `dir` | `lt` | `swap` | 输出 (lo, hi) |
|:-:|:-:|:-:|:-:|
| 1 (DESC) | 1 | 1 | (b, a) |
| 1 (DESC) | 0 | 0 | (a, b) |
| 0 (ASC)  | 1 | 0 | (a, b) |
| 0 (ASC)  | 0 | 1 | (b, a) |

`cmp_swap_const_dir(direction)` 把 `direction` 当编译期常量，省一层 XOR。所有
Stage A 和 Stage B merge 的调度方向都是编译期固定（DESC 或交替），调用的就是
这个版本。

### 4.2 `fp_lt(a32, b32, fmt_sel)`

```
       a32 (32 bit)                b32 (32 bit)         fmt_sel (2 bit)
          │                            │                       │
          ▼                            ▼                       │
    ┌─────────────────┐      ┌─────────────────┐               │
    │ key_bf16(a16)   │      │ key_bf16(b16)   │  ──ult──→ lt_bf16
    │ key_fp16(a16)   │      │ key_fp16(b16)   │  ──ult──→ lt_fp16
    │ key_fp32(a32)   │      │ key_fp32(b32)   │  ──ult──→ lt_fp32
    └─────────────────┘      └─────────────────┘               │
                                                               ▼
                                                    fmt_sel mux → lt
```

3 套 NaN→-∞ + sign-magnitude → monotone unsigned 的 key 转换并联生成；
比较结果三选一。每个 `cmp_swap` 只有一个 comparator 网络。

### 4.3 `bitonic_merge_2p_full(P)` 接线（valley bitonic）

```
lane[i]      = A[i]            i ∈ [0, P)        # A desc (SRAM row 或 -inf 行)
lane[P + j]  = B[P-1-j]        j ∈ [0, P)        # reverse(B), 升序 → valley bitonic
```

Schedule 共 `log2(P) + 1` 层，所有层 dir=DESC：

```
layer 0:  stride = P     pairs (i, i+P)            i ∈ [0, P)
layer 1:  stride = P/2   pairs within each P-block
...
layer log2(P):  stride = 1   pairs (i, i+1)        even i
```

P=256: 9 层 × 256 cmp_swap = **2304 cells**。输出全部保留：
- lane[0..P-1] = top P（写回 SRAM）
- lane[P..2P-1] = bottom P（变成下一行的 carry）

### 4.4 资源 / 时序预算（P=256, K_MAX=4096, idx_w=12）

| 项目 | 值 |
| --- | --- |
| Stage A cmp_swap | 36 layers × 128 = **4608** |
| Stage B merge cmp_swap | 9 layers × 256 = **2304** |
| Stage A latency | 36 cy |
| Stage B per-chunk cycles | `rows_used + 1` |
| Running SRAM | 16 rows × 256 × (32 + 12) = 180 Kb ≈ 22.5 KB |
| `init_done` FF | 16 |
| `k_in_w` 端口 | 13 bits |
| `drain_addr_w` 端口 | 4 bits |

吞吐（chunk = 256 elements）：

- K ≤ P （rows_used=1）：**2 cy / chunk** → P/2 elements/cy → N=1M 约 8K cy ≈ 8 µs @ 1 GHz
- K = 4096 （rows_used=16）：**17 cy / chunk** → N=1M 约 69.6K cy ≈ 70 µs @ 1 GHz
- 固定延迟：Stage A 流水 36 cy + 首 chunk 的 Stage B `rows_used` cy → 第一个有效输出 ≈ 38..52 cy 后到达

## 5. K = 4 数字 walkthrough（统一路径，rows_used = 1）

设 `P = 4`, `k_in = 4`（runtime），`fmt_sel = FP_FP32`。输入两个 chunk：

```
chunk 0 = [3.0, 1.0, 4.0, 1.0]   idx [0, 1, 2, 3]
chunk 1 = [5.0, 9.0, 2.0, 6.0]   idx [4, 5, 6, 7]
```

**Stage A**（4-sort，全流水，descending）：

```
  chunk 0 in  : 3 1 4 1
  Stage A out : 4 3 1 1

  chunk 1 in  : 5 9 2 6
  Stage A out : 9 6 5 2
```

**Stage B**（streaming，rows_used=1，第一次访问 SRAM row 0 时 `init_done[0]=0` →
读 mux 给出 `[-∞, -∞, -∞, -∞]`）：

```
cycle (chunk 0): merge([-∞,-∞,-∞,-∞], [4,3,1,1])
                 → top P  = [4,3,1,1] → write SRAM[0], init_done[0] ← 1
                 → bot P  = [-∞,-∞,-∞,-∞] (discarded for rows_used=1)

cycle (chunk 1): id[0]=1 → read real SRAM[0] = [4,3,1,1]
                 merge([4,3,1,1], [9,6,5,2])
                 → top P  = [9,6,5,4] → write SRAM[0]

topk_vals @ drain_addr=0 : [9, 6, 5, 4]
```

golden = `sorted([3,1,4,1,5,9,2,6], desc)[:4] = [9,6,5,4]`，匹配 ✓。

更详细的图见 `figures/walkthrough_k4.png`。

## 6. 浮点比较（运行时 fmt_sel）

```python
def fp_lt_py(a_bits, b_bits, fmt_sel):
    fmt = fmt_of_sel(fmt_sel)              # 0=bf16, 1=fp16, 2=fp32
    mask = (1 << fmt.width) - 1
    return 1 if (fp_to_unsigned_key(a_bits & mask, fmt)
               < fp_to_unsigned_key(b_bits & mask, fmt)) else 0


def fp_to_unsigned_key(bits, fmt):
    if is_nan(bits, fmt):
        bits = neg_inf_bits(fmt)            # NaN 折叠为 -∞
    if sign(bits) == 0:
        return bits ^ (1 << sign_bit)       # 翻转符号位
    return ~bits & mask                     # 负数整体取反
```

性质：

- 严格单调：`fp_to_unsigned_key(a, fmt) < fp_to_unsigned_key(b, fmt)` ↔ `float_of(a, fmt) < float_of(b, fmt)`，按 NaN→-∞ 全序。
- ±0 在该全序里 `-0 < +0`（与 IEEE 754 的「相等」不同；对 Top-K 无害）。
- subnormal 与 ±∞ 自然处理。

硬件版本（`fp_lt(a32, b32, fmt_sel)`）把这套转换实例化三份（bf16/fp16/fp32），
比较结果由 `fmt_sel` mux 三选一。**每个 cmp_swap 里只放一个 comparator 网络
（比较器面积约 +50%，仍由 SRAM 主导芯片面积）。**

软件参考与硬件实现位字一致（`tool.py`），自测见 `selftest/test_fp_compare.py`。

## 7. 参数与运行时接口

### 7.1 编译期参数（传给 `compile_cycle_aware(build, ...)`）

| 名字 | 默认值 | 说明 |
|---|---:|---|
| `P`        | 256 | chunk 宽度 = bus 宽度 = Stage A sort 宽度，2 的幂 |
| `K_MAX`    | 4096 | K 上限；决定 SRAM 行数 = K_MAX/P，2 的幂，K_MAX % P == 0 |
| `idx_w`    | 12 | lane index 位宽（默认足够 1M 元素） |

### 7.2 运行时端口

输入：

| 端口 | 宽度 | 说明 |
|---|---|---|
| `chunk_vals`         | `P × VAL_W` (`= P × 32`) | P 个 val 打包，lane 0 在 LSB；bf16/fp16 用每个 lane 的低 16 位 |
| `chunk_idxs`         | `P × idx_w`              | P 个 idx 打包 |
| `valid_in`           | 1 | 当前 chunk 有效；`ready_out=1` 时拉一拍吞一个 chunk |
| `fmt_sel`            | 2 | 0=bf16, 1=fp16, 2=fp32；整个 session 内保持稳定 |
| `k_in`               | `k_in_w(K_MAX)` （=`ceil(log2(K_MAX+1))`） | 运行时 K ∈ [1, K_MAX] |
| `topk_sb_drain_addr` | `log2(K_MAX/P)` | 选哪一行 running SRAM 暴露到输出 |

输出：

| 端口 | 宽度 | 说明 |
|---|---|---|
| `topk_vals`     | `P × VAL_W` | `drain_addr` 指定那一行的 P 个 val；row 0 是全局 Top-P |
| `topk_idxs`     | `P × idx_w` | 对应 idx |
| `running_valid` | 1 | 至少 1 个 chunk 完整 absorb 之后 sticky 为 1 |
| `ready_out`     | 1 | 1 = 可接受新 chunk；issue throttle 在 `rows_used + 1` cy 之间会拉 0 |

### 7.3 上层协议

- 整个 session 内保持 `fmt_sel` 与 `k_in` 稳定（中途换会污染 running register，
  但硬件不会卡死）。
- 模块在**复位后总是 ready**，没有 cfg_valid / sweep 启动握手——`init_done` 在
  每行第一次写入时自动置 1，未写过的行读出来就是 `neg_inf(fmt_sel)` 的整行。
- 读完整 Top-K 需要驱动 `drain_addr` 遍历 `rows_used` 行（每行 P 个元素），
  IDLE 期间一拍一行；row 0 是 top-P。

## 8. 使用示例

### 8.1 命令行：编译目标尺寸

```bash
cd /path/to/pyCircuit
PYTHONPATH=compiler/frontend python designs/examples/topk/topk.py
```

会编译 (P=16, K_MAX=64) 到 (P=256, K_MAX=4096) 几个尺寸并打印 MLIR 大小、Stage
A latency、`k_in_w`。

### 8.2 算法验证

```bash
PYTHONPATH=compiler/frontend python designs/examples/topk/tb_topk.py
```

跑 3 fmt × K ∈ {1, 7, 8, 256, 257, 1024, 4096} × N 到 1M 全部对 numpy golden
匹配，含 7 类边界用例（全等 / 严格升降 / 全 NaN / 全 ±∞ / 混合符号）。

### 8.3 渲染原理图

```bash
python designs/examples/topk/figures/draw_arch.py
```

输出：
- `figures/topk_overview.png`         — 单一统一架构
- `figures/stage_a_full_p_sort.png`   — Stage A 全 P-sort 网络
- `figures/merge_2p_full_cell.png`    — `bitonic_merge_2p_full(P)` 内部
- `figures/stage_b_unified_fsm.png`   — Stage B FSM + SRAM + `init_done` + carry
- `figures/walkthrough_k4.png`        — K=4 数字 walkthrough
- `figures/topk_all.png`              — 6 合一总览

### 8.4 嵌入到上层模块

```python
from designs.examples.topk.topk import build as topk_build
from pycircuit import compile_cycle_aware

circuit = compile_cycle_aware(
    topk_build,
    name="my_topk",
    P=256, K_MAX=4096, idx_w=12,
)
mlir = circuit.emit_mlir()
```

注意：`build()` 不再吃 `K` 或 `fmt_name` 编译期参数——这两个改在运行时端口
`k_in` / `fmt_sel` 上设。

## 9. 验证策略

四层 check：

1. **调度自检**（`selftest/test_bitonic_schedule.py`）：Batcher 排序 N=2..256 对 `sorted(reverse=True)` 完全匹配；`bitonic_merge_2p_full(P)` 对随机输入输出完整 2P 排序。
2. **比较器自检**（`selftest/test_fp_compare.py`）：三种 fmt × 2000 随机 + 10 特殊用例（±0/±∞/NaN/subnormal/同值）全部对 Python `<` 一致。

```bash
cd designs/examples/topk
PYTHONPATH=../../../compiler/frontend python selftest/test_all.py
```
3. **小规模 RTL 烟测**（`tb_topk.py`，P=4 / K_MAX=16 / K=4）：CycleAwareTb 喂 2 个 chunk，golden 来自下面的 RTL-bit-exact 软件模型 `sw_topk_unified_pairs`。每次推 push 都在 Verilator 上跑一遍。
4. **大规模 RTL workload**（`tb_topk_large_{bf16,fp16,fp32}.py`，P=32 / K_MAX=128 / K=64 / N=4096）：3 个独立 testbench，每个塞 128 个 chunk × 32 lane = 4096 个随机 fp 值，分别采 row 0（top-32）和 row 1（next-32），完整覆盖 K>P 的 SRAM 滚动合并路径。

```bash
# 端到端 4 个 RTL tb（约 20s 编译 + 60s 仿真 / 个）
PYTHONPATH=compiler/frontend python -m pycircuit.cli build \
    designs/examples/topk/tb_topk.py \
    --out-dir /tmp/topk_smoke --target verilator --run-verilator
for fmt in bf16 fp16 fp32; do
  PYTHONPATH=compiler/frontend python -m pycircuit.cli build \
    designs/examples/topk/tb_topk_large_${fmt}.py \
    --out-dir /tmp/topk_large_${fmt} --target verilator \
    --logic-depth 1024 --run-verilator
done
```

软件模型 `sw_topk_unified_pairs` **位精确**镜像 RTL：

- Stage A：用 `gen_sort_schedule_desc(P)` 跑一个 P-element bitonic sort，且 `cmp_swap` 的并列 (tie) 行为完全照搬 RTL — DESC `swap = lt`（平局保留 lane A 在 lo），ASC `swap = ~lt`（平局把 lane B 放到 lo）。Python 的 `sorted(stable=True)` 在低精度浮点（bf16/fp16）有大量并列时**不**满足这个语义，所以会 idx 不匹配。
- Stage B：`for r in range(rows_used): row = init_done[r] ? rows[r] : [-∞]*P; merged = full_merge_2p_bitonic_hw(row, carry); rows[r] = merged[:P]; carry = merged[P:]; init_done[r] = True`

## 10. 主要权衡 vs 旧的多次编译方案

- **+ 一个硬件**：彻底消除「按 (fmt, K) 跑多次综合」的流程。一份 GDS 跑所有 fmt 和所有 K。
- **+ 运行时 fmt**：一颗芯片同时跑 bf16/fp16/fp32，不用 re-spin。
- **+ 透明初始化**：无 cfg_valid / startup sweep，复位后立刻 ready。
- **− K ≤ P 吞吐回归**：旧 reg-array 快路径是 1 cy/chunk；新统一架构是 2 cy/chunk（issue throttle + 1 cy IDLE + 1 cy MERGE_LOOP）。芯片面积仍由 SRAM 主导，但延迟翻倍。
- **− Comparator 面积**：每个 `cmp_swap` 大约大 50%（3 路并联 monotone-key + fmt_sel mux）。
- **= K > P 延迟**：仍是 `rows_used + 1` cy/chunk，与旧版基本相同。

如果 K ≤ P 的吞吐回归后面变成问题，下一版可以加一个「row 0 影子寄存器」的融合
1 cy 路径（row 0 走组合读写，row ≥ 1 才落 SRAM）。本次重构不做。

## 11. 已知限制

- `k_in = 0` 行为未定义（硬件不会卡死，但 running 结果无意义）。上层须给 ≥ 1。
- `mask` 输入暂不支持：上游需自行把屏蔽位置打成 -∞ 的位串。
- `fmt_sel` / `k_in` 中途切换不被支持（会污染 running SRAM）。上层需在 reset 之后再变更。
- Stage B 用 `m.sync_mem`（**1-cycle 寄存读延迟**）作 running SRAM。MERGE_LOOP 内部做了 `raddr` 预取（cycle T 驱动 raddr=rcnt(T+1) 给 cycle T+1 的 merge 用），并把 raddr 自身寄存为 `raddr_d` 给 `init_done` lookup 用，保证 `init_done_bit` 索引到的就是 `sram_rdata` 当拍真正反映的那一行。Merge 关键路径仍是 `log2(2P)+1` 层 cmp_swap（含读出寄存器后那一拍 + 一层 init-done mux）。

## 12. Verilator 工具链注意事项

- **`--unroll-count` 必须 ≥ 1024（已在 `compiler/frontend/pycircuit/cli.py` 默认开启）**：`pyc_sync_mem.v` 的逐字节写入是 `for (i = 0; i < STRB_WIDTH - 1; i = i + 1) mem[wa][8*i +: 8] <= wdata[8*i +: 8];`。当 `STRB_WIDTH` 超过 Verilator 默认 `--unroll-count=64` 时，循环不会展开，循环内的 NBA 会被**静默丢弃**（只剩循环外那一 lane 真正被写）。在 P=32 / VAL_W=32 / idx_w=12 → STRB_WIDTH=176 的 tb 上，这会让 `mem[0]` 只更新最高 16 位、其他位仍是复位值，row 0 vals 直接对不上。
- **`-Wno-BLKLOOPINIT`**：上述按字节 `for` 循环里有 NBA，Verilator 会发 `BLKLOOPINIT` 警告，已在 CLI 里关掉（行为已通过手动展开等价）。
