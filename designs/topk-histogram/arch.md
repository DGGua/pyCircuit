# Radix-Based Top-K FPGA 加速器设计文档

> 输入：1024 个 IEEE 754 fp32 元素
> 输入端口位宽：512 Byte = 4096 bit（即每周期 128 个 fp32）
> 输出端口位宽：512 Byte（同上）
> 算法基础：参考 `vf_topk.h` 的 4 轮 8-bit Radix Select

---

## 1. 设计目标与关键参数

| 项目 | 取值 |
|---|---|
| 数据类型 | IEEE 754 float32（含负数 / NaN） |
| 输入元素数 N | 1024（可参数化） |
| Top-K 范围 K | 1 ~ 1024（运行时配置） |
| 输入数据并行度 | 128 × fp32 / cycle = 4096 bit |
| 输入数据时序 | **req-触发的固定 8-cycle burst，无反压** |
| 输出端口位宽 | 4096 bit |
| 输出数据时序 | **FPGA 主动发 req + 固定 8-cycle burst，无反压** |
| 目标频率 | 400 ~ 600 MHz（取决于工艺/器件） |
| 总延迟（in_req → 末拍 DRAIN） | **78 ~ 85 cycle**（小 K 端 78 cy；K=900 EQ 跑满 85 cy）≈ 156 ~ 170 ns @500 MHz |
| 吞吐（单实例） | 1 个 Top-K / ~85 cycle |
| 吞吐（ping-pong） | 1 个 Top-K / **61 ~ 68 cycle**（受 COMPUTE 段约束，LOAD/DRAIN 可完全重叠）|

设计原则：

1. **完全复用软件版的 4 轮 radix select 算法**，把 8 cycle 的 SRAM 读窗口作为整个数据通路的节拍单位。
2. **接口端无反压**：输入侧上游发出 `in_req` 后，内部必须无条件吃下 8 cycle 的数据 → 由 `data_sram` 写口（前置 `fp_to_key` 组合 shim）一拍一行直写到主存储；输出侧 FPGA 主动发起 `out_req` 后，内部必须无条件供出 8 cycle 的数据 → 必须有内置 `output_buf` 做 rate/format 转换。
3. **不需要独立的输入 buffer**：算法本来就要把数据放进 `data_sram` 让 radix 反复读 6 次，写口直接吃 burst 即可；输出端因为 `filter_compact` 的写入是不规则的 0~128 lane/cy，必须用 `output_buf` 做 rate/format buffer。两者都可以做 ping-pong（§11），让"接收下一帧"和"输出当前帧"同时进行。

---

## 2. 顶层架构

```
                  ┌──────────────────────────────────────────────────┐
                  │                  topk_top                        │
                  │                                                  │
in_req ───────────►│   recv 子机 (隶属 topk_ctrl):                    │
                  │     • waddr counter (0..7)                      │
                  │                  │                               │
in_data[4096b]────►│ ┌─────────────────┐  4096b   ┌────────────────┐ │
(8 cycle burst)   │ │ fp_to_key × 128 │ ────────►│   data_sram     │ │
                  │ │ (组合, NaN/±符号)│          │ (128bank×8×32, │ │
                  │ └─────────────────┘          │  存 sortable key)│ │
                  │                              └────────┬────────┘ │
                  │                                       │ 4096b/cy │
                  │   ┌───────────────────────────────────┘  读出    │
                  │   ▼                                              │
                  │ byte_select ──► fp已是key, 直接进入数据通路       │
                  │   │   │                                          │
                  │   ▼   ▼                                          │
                  │ mask_update ──► histogram_engine                 │
                  │       │                │                         │
                  │       │                ▼                         │
                  │       │         cumsum_threshold ─► target_bin_R │
                  │       │                │                         │
                  │       └──── (反馈到下一轮 mask)                  │
                  │                        │                         │
                  │                        ▼                         │
                  │                 kth_compose ─► kth_key           │
                  │                        │                         │
                  │  ┌─────────────────────┘                         │
                  │  ▼                                               │
                  │ filter_compact ──► key_to_fp                     │
                  │       │                  │                       │
                  │       └──► output_buf ◄──┘                       │
                  │            (K个value+idx                         │
                  │             配 out_req 控制)                     │
                  │              │                                   │
                  │              ▼                                   │
                  │         out_req                                  │
                  │         out_value[4096b]   out_index_data[4096b] │
                  │         out_valid_mask[128]                      │
                  │         (8 cycle burst)                          │
                  │                                                  │
                  │   topk_ctrl (主 FSM + recv 子机, 协调 LOAD /     │
                  │             计算 / output_buf, 处理 ping-pong)   │
                  └──────────────────────────────────────────────────┘
```

> 设计说明：**没有独立的 input buffer 模块**。`in_data` 经过 128 lane 的 `fp_to_key` 组合逻辑后，由 `topk_ctrl` 内一个 3-bit 写指针在 8 个 cycle 内顺序直写 `data_sram`。`data_sram` 自身就是这一段数据的存储介质，无需再加一级 FIFO。
>
> **任务/帧 ID**：本设计不提供任务 ID 透传通道（没有 `in_index`/`out_index`）。上游若需要把请求与响应配对，请在外部按 `in_req` → `out_req` 的 FIFO 顺序自行维护。

整个流水分为三大段：

| 段 | 角色 | 时钟域占用 |
|---|---|---|
| **LOAD 段** | `data_sram` 写口接住 8-cycle burst（`fp_to_key` 在线 → 直写主存储） | 8 cy |
| **计算段** | `data_sram` 读口 + radix + filter | RADIX + FILTER，约 65 ~ 80 cy |
| **发送段** | `output_buf` 持有结果，等到位主动发 `out_req`，burst 出去 | DRAIN，固定 8 cy |

---

## 3. 顶层端口规范

### 3.1 端口定义

```verilog
module topk_top #(
    parameter int N            = 1024,        // 输入元素数
    parameter int LANE_NUM     = 128,         // 并行通道数 (= 4096b / 32b)
    parameter int LANE_BYTES   = 4,           // fp32 = 4B
    parameter int K_MAX_BITS   = 11,          // ⌈log2(N+1)⌉
    parameter int ELEM_IDX_BITS= 32,          // 每个元素的原始位置 index 位宽 (per-lane)
    parameter int BURST_LEN    = 8            // 输入/输出固定突发长度
)(
    // ───── 时钟 / 复位 ─────
    input  logic                          clk,
    input  logic                          rst_n,

    // ───── 配置寄存器（在 in_req 之前必须保持稳定）─────
    input  logic [K_MAX_BITS-1:0]         cfg_topk,        // 1 ≤ K ≤ N；K=0 行为未定义，硬件内部夹紧到 1
    output logic                          status_busy,     // 拉高表示内部正在计算

    // ───── 输入接口（slave, req-触发的固定 burst, 无反压）─────
    input  logic                          in_req,          // 单脉冲, 表示"下一拍起 BURST_LEN 个有效"
    input  logic [LANE_NUM*32-1:0]        in_data,         // 128 × fp32, 在 in_req 后连续 8 拍有效；
                                                           //   元素原始位置 index 由位置派生:
                                                           //     elem_idx[beat][lane] = beat * LANE_NUM + lane
                                                           //   beat ∈ [0, BURST_LEN), lane ∈ [0, LANE_NUM)

    // ───── 输出接口（master, FPGA 主动发起, 固定 burst）─────
    output logic                          out_req,         // 单脉冲, 表示"下一拍起 BURST_LEN 个有效"
    output logic [LANE_NUM*32-1:0]        out_value,       // 128 × fp32 (sortable key 已逆变换回 fp32)
    output logic [LANE_NUM*32-1:0]        out_index_data,  // 128 × uint32 元素原始位置 index
                                                           //   只低 ⌈log2 N⌉=10 bit 有效, 高位补 0
                                                           //   注：此 index 是"该元素在本次输入流里的位置 0..N-1"，
                                                           //       与外部的任务/帧 ID 是不同概念。
    output logic [LANE_NUM-1:0]           out_valid_mask   // 每 lane 是否有效, 最后一拍可能尾部为 0
);
```

> **K=0 行为**：cfg_topk 在采样时硬件夹紧到 1（`cfg_topk_eff = (cfg_topk == 0) ? 1 : cfg_topk`），避免下游 bottomK 计算溢出。
>
> **NaN 行为**（详见 §4.2.3 / §4.2.4）：
> - 输入含 NaN 时，输出元素中可能包含 NaN，**bit pattern 不保证与输入完全一致**，但"是 NaN"性质守恒。
> - sortable key 空间中所有 NaN 排在所有有限数（含 ±∞）之前，所以 top-K **会优先选中 NaN**。若业务不希望 NaN 进入 top-K，请上游先过滤。

### 3.2 接口时序

#### 输入侧（slave，必须无条件接收）

```wavedrom
{ "signal": [
  { "name": "clk",      "wave": "p........" },
  { "name": "in_req",   "wave": "10......." },
  { "name": "in_data",  "wave": "x========", "data": ["beat0","beat1","beat2","beat3","beat4","beat5","beat6","beat7"] }
],
  "head": { "text": "输入侧：in_req 拉高 1 拍 → 下一拍起 8-cycle burst", "tick": 0 },
  "foot": { "text": "128 fp32 × 8 beat = 1024 fp32" }
}
```

- `in_req` 高 1 拍即触发，**绝对不允许反压**（无 ready 信号）
- 数据从 `in_req` 的下一拍起连续 8 拍有效（首拍即 beat0）
- 输入侧不携带任务/帧 ID；如需把请求与响应配对，由上游按 `in_req` → `out_req` 的 FIFO 顺序自行维护
- 如果 `in_req` 在内部正忙时再次到来：见 §3.4 ping-pong 规则

#### 输出侧（master，FPGA 主动发起）

```wavedrom
{ "signal": [
  { "name": "clk",            "wave": "p........" },
  { "name": "out_req",        "wave": "10......." },
  { "name": "out_value",      "wave": "x========", "data": ["beat0","beat1","beat2","beat3","beat4","beat5","beat6","beat7"] },
  { "name": "out_index_data", "wave": "x========", "data": ["beat0","beat1","beat2","beat3","beat4","beat5","beat6","beat7"] },
  { "name": "out_valid_mask", "wave": "x=======3", "data": ["all 1","all 1","all 1","all 1","all 1","all 1","all 1","low 4"] }
],
  "head": { "text": "输出侧：out_req 拉高 1 拍 → 下一拍起 8-cycle burst", "tick": 0 },
  "foot": { "text": "波形以 K=900 为例：beat0..beat6 全 128 lane 有效，beat7 仅低 4 lane 有效（900 − 7×128 = 4）。mask 标签为抽象描述，真实信号为 128 bit。" }
}
```

- FPGA 内部完成 Top-K 后主动拉高 `out_req` 1 拍
- 之后 8 拍连续输出，下游必须无条件接收
- `out_value` / `out_index_data` 同步配对
- **输出顺序**：按 `filter_compact` 扫描 `data_sram` 的顺序（先 row 再 lane 内 prefix-sum 位置）紧凑排列，**不保证按 value 排序**。下游若需排序，请在外部追加一个 P-lane sort。
- `out_valid_mask`：当 K 不是 128 的整数倍时，最后一拍只有低位有效；不足 K 的位置全 0
  - 例如 K=200：beat0 全 128 lane 有效，beat1 低 72 lane 有效（共 200），beat2~7 全 0
  - 例如 K=900：beat0..beat6 全 128 lane 有效（896），beat7 低 4 lane 有效（共 900）
- `out_valid_mask` 与每拍的 lane 数语义等价于 `total_count`（11-bit 元素总数）；两者应一致，由 `output_buf.send` 子机统一生成

### 3.3 多任务标识

本设计不提供任务/帧 ID 透传通道。若上游需要把请求与响应配对：

- 推荐策略：上游按 `in_req` 发出顺序、本模块按 `out_req` 完成顺序构成天然 FIFO；上游侧维护一个 task-ID 队列，`out_req` 来时弹出对应 ID
- 若启用 ping-pong（§3.4 / §11），输出顺序仍保持 FIFO，不会乱序

### 3.4 多任务流水规则（ping-pong 启用时）

- `status_busy = 0` 时允许接受新的 `in_req`
- 启用 ping-pong 后（见 §11），允许在当前任务还在计算时接受下一帧
- 输出顺序保持 FIFO：先进先出

---

## 4. 内部模块切分

### 4.1 模块清单

| 模块 | 实例数 | 关键功能 |
|---|---|---|
| **`data_sram`**        | 1（128 bank × 8 深 × 32 bit）| 主数据存储（已是 sortable key 形态）；其写口在 LOAD 阶段直接吃下 `in_data` burst |
| `fp_to_key`            | 128 lane（组合）| `data_sram` 写口前 shim：fp32 → sortable key（NaN / 正负号处理）|
| `byte_select`          | 128 lane | 从 32-bit key word 抽取当前轮的字节 |
| `mask_reg`             | 1（1024-bit 寄存器）| 当前活跃元素 mask |
| `mask_update`          | 1 | 用 `target_bin_R` 收紧 mask |
| `histogram_engine`     | 1 | 256-bin 字节直方图（128 lanes 并行）|
| `cumsum_threshold`     | 1 | 直方图累加 + 阈值比较，输出 `target_bin_R` |
| `kth_compose`          | 1 | 4 字节 idx 拼接成完整 32-bit kth_key |
| `filter_compact`       | 1 | GT/EQ 比较 + 前缀和 compaction，结果写入 `output_buf` |
| `key_to_fp`            | 128 lane（组合）| sortable key → fp32 bit pattern |
| **`output_buf`**       | 1 | 持有 K 个 (value, index)；调度 `out_req` 主动 burst 输出 |
| `topk_ctrl`            | 1 | 顶层 FSM（含 recv 子机：3-bit waddr counter），协调 LOAD / 计算 / `output_buf` 三段 |

注：
- **没有独立的 input buffer**。`fp_to_key` 是 `data_sram` 写口前的纯组合 shim，存储责任直接落在 `data_sram` 上（这块存储反正算法要用 4 轮 radix + filter 共 6 次读，本就必备）。
- `key_to_fp` 同样作为组合 shim，嵌在 `filter_compact` → `output_buf` 的写出链路上。
- 这样 `data_sram` 和 `output_buf.value_ram` 里**全程是 sortable key / fp32 (已还原) 各自的稳定形态**，下游比较和发送都无需再变换。

### 4.2 关键模块详细设计

#### 4.2.1 `data_sram`（主存储 + 写口直接吃 burst）

**职责**：作为算法主存储（radix 4 轮 + filter 2 轮共 6 次读）；同时其写口在 LOAD 阶段无条件吃下 8-cycle 输入 burst，把 fp32 经组合 `fp_to_key` 后存为 sortable key。

```
                ┌─────────────────────────────────────────────────┐
                │                  data_sram                      │
                │                                                 │
  in_req ──────►│  ┌─────────────────┐                            │
                │  │ topk_ctrl.recv  │                            │
                │  │  3-bit waddr    │── waddr (0..7) ──┐         │
                │  │  counter        │                  │         │
                │  │                 │── we ────────────┤         │
                │  └─────────────────┘                  │         │
                │                                       ▼         │
  in_data ─────►│  ┌─────────────────┐   4096 b   ┌───────────┐  │
   [4096 b]    │  │ fp_to_key × 128 │ ─────────►│  storage   │  │
                │  │ (组合, NaN/±符)  │            │ 128 bank× │  │
                │  └─────────────────┘            │  8 deep × │  │
                │                                 │  32 bit   │  │
                │                                 └─────┬─────┘  │
                │                       sram_raddr (3) │ 4096 b │
                │            ◄──────────────────────────┘ /cy   │
                │            ┌──────────────────────────────►   │
                │            ▼                                  │
                │   下游 byte_select / filter 读                  │
                └─────────────────────────────────────────────────┘
```

- **结构**：128 bank × 8 entry × 32 bit。单 cycle 读出 4096 bit（一行 = 128 lanes）。
- **写口**：在 LOAD 阶段被 `topk_ctrl` 的 recv 子机独占，每拍写一行（4096 bit）。
- **读口**：RADIX / FILTER 阶段所有 bank 同地址并行读出。
- **存储内容**：`fp_to_key` 之后的 sortable uint32 key（不是原始 fp32！）。
- **存储介质推荐**（按优先级）：
  1. **distributed RAM / LUTRAM**（首选）：128 bank × 8 deep × 32 bit ≈ 128 × 32 个 RAM32X1S = 4K LUT6（in RAM mode），最自然贴合本设计
  2. **纯寄存器实现**（FF）：1024 × 32 = 32K FF，对中等 FPGA 完全可行，时序最干净
  3. **不推荐 BRAM**：BRAM36K 是 1024×36 bit，8 行的实际利用率 < 1%，1~2 块全浪费。仅在 LUTRAM 紧张时才退而求其次
  - 总容量 = 32 Kb；实际综合时按上面 1/2 选一种

```
data_sram[addr][lane] : 32 bit (sortable key)
addr ∈ [0, 7],  lane ∈ [0, 127]
```

**接收子机（写口侧，逻辑上属于 `topk_ctrl`）**：

```
state IDLE       : 等待 in_req
state RECEIVING  : in_req 拉高的下一拍进入, 用 burst counter 0..7 作为 waddr
                   每拍 we=1 写一行 (4096 bit) 到 data_sram, 同时跑 fp_to_key
state DONE       : burst counter 满 8, 拉高 load_done, 回到 IDLE
```

**关键点**：
- 没有 ready 反压 → 内部不能有任何"卡住"的可能，写入 SRAM 是确定性的 1 拍延迟。
- `fp_to_key` 是纯组合逻辑（128 lane × 几级 LUT），不影响时序闭合。
- 如果担心高频下 `fp_to_key` 是关键路径，可以把它放在写口前一级寄存器后面（增加 1 cy 延迟，但仍在 8 cy 之内）。
- **没有独立的 input buffer**：burst 数据不在路径上停留，直接进入 `data_sram`；下游 RADIX / FILTER 反正都要从这里读数据。

**写口资源**：3-bit counter + 128 lane `fp_to_key` ≈ 1.5K LUT, 50 FF（已与 `data_sram` 一起列在 §7 资源表里）。

如果做 ping-pong（§11），把存储实例化两份：`data_sram_A` / `data_sram_B`，写口前加一位 bank-select 复用。

#### 4.2.2 `output_buf`（输出缓冲，含 key_to_fp + 主动 burst 发送）

**职责**：在 `filter_compact` 阶段把 K 个结果（value 已经过 key_to_fp 还原成 fp32）紧凑写入；之后由 FSM 触发主动 8-cycle burst 输出。

```
                     ┌─────────────────────────────────────────────────┐
                     │                output_buf                       │
                     │                                                 │
filter_compact ──►───┤ ┌─────────────────────────────────────────────┐ │
  (value_key+idx,    │ │   写口: 多 lane 紧凑写                       │ │
   每周期 0~128 个)  │ │   - 内部按写指针 wptr 顺序 (0..1023) 排列    │ │
                     │ │   - 数据已经 key_to_fp 还原成 fp32           │ │
                     │ └─────────────────────────────────────────────┘ │
                     │                                                 │
                     │ ┌─────────────────────────────────────────────┐ │
                     │ │   value_ram : 128 bank × 8 deep × 32 bit    │ │
                     │ │   index_ram : 128 bank × 8 deep × 32 bit    │ │
                     │ │   两块对齐, 与 data_sram 结构相同            │ │
                     │ └────────────────┬────────────────────────────┘ │
                     │                  │                              │
filter_done ──►──────┤ ┌─────────┐      │                              │
                     │ │ send    │      ▼                              │
                     │ │  FSM    │   读口 (4096 bit/cy)                │
                     │ └────┬────┘      │                              │
                     │      │           ▼                              │
                     │      ├───────► out_req                          │
                     │      ├───────► out_value      [4096b]           │
                     │      ├───────► out_index_data [4096b]           │
                     │      └───────► out_valid_mask[128]              │
                     └─────────────────────────────────────────────────┘
```

**写入端**：来自 `filter_compact`，不规则突发（GT 阶段 0~128 lane 有效，EQ 阶段 0~128 lane 有效）。需要一个写指针 `wptr` 累加压紧。

**写入端实现细节**（决定了真实 LUT 开销）：

每周期收到本周期 `predicate[128]` 及对应的 `value/idx`，要落到 `value_ram / index_ram` 的物理 `(bank, addr) = ((wptr+k) % 128, (wptr+k) / 128)`，其中 `k = 0..127` 是本周期通过 prefix-sum 紧凑后的输出 lane 序号。

实现链路：

1. **128 路 7-bit prefix-sum** on `predicate[128]` → 每个被选中的输入 lane 给出其在本周期紧凑流中的位置 `pos[lane] ∈ [0, 127]`
2. **写地址生成**：`waddr[k] = wptr + k`（k 为紧凑后的输出序号）
3. **128-way 可变 barrel shifter / crossbar**：把 128 个紧凑后的 (value, idx) 按 `wptr mod 128` 循环对齐到 128 个 bank 的写端口
4. **per-bank write-enable**：当本周期紧凑后的某 bank 没有写入时拉低 we；横跨两行 bank（`wptr mod 128 + n > 128`）时，bank 编号回卷到的那些 bank 的 addr 取 `(wptr / 128) + 1`，其余取 `wptr / 128`
5. **wptr 更新**：`wptr_next = wptr + popcount(predicate)`；GT pass 结束后保留 `gt_count = wptr`；EQ pass 起始 `wptr` 继续累加

**读取端**：FSM 触发后固定 8 cycle 全行 burst 输出。

**send FSM**：

```
state IDLE     : 等待 filter_done
state ISSUE    : 拉高 out_req 1 拍
state SENDING  : 接下来 8 拍, 用 raddr counter (0..7) 读 value_ram / index_ram
                 同时根据 K 总数生成 out_valid_mask (前 K 个为 1, 其余为 0)
state CLEAR    : 清掉 wptr 等状态, 回到 IDLE
```

**`out_valid_mask` 生成**：
- 写指针在 filter_compact 累加得到 `total_count = K`（GT_count + EQ_count）
- 在 SENDING 阶段，第 t 拍 (t=0..7)：
  ```
  if      ((t+1)*128 <= total_count) out_valid_mask = 128'hFFFF...FFFF;
  else if (t*128     >= total_count) out_valid_mask = 128'h0;
  else                               out_valid_mask = (1 << (total_count - t*128)) - 1;
  ```
  即"前 total_count 位为 1，后面填 0"。

**资源**（修订：原 1.5K LUT 严重低估了紧凑写入逻辑）：
- value_ram + index_ram = 2 × 32 Kb = 64 Kb；存储介质推荐与 `data_sram` 一致（LUTRAM 优先，FF 次之，BRAM 浪费）
- write pointer（11 bit）+ read counter（3 bit）+ valid_mask 生成器 ≈ 0.5K LUT
- **128 路 7-bit prefix-sum** ≈ 1.0 ~ 1.5K LUT
- **128-way 可变 barrel shifter / crossbar**（128 输入 × (32+10) bit，按 `wptr mod 128` 旋转）≈ 4 ~ 6K LUT
- **per-bank write-enable 生成 + 行号回卷加 1** ≈ 0.3K LUT
- **小计**：紧凑写入硬件 ≈ **5 ~ 8K LUT**（取决于综合工具对 barrel shifter 的实现）；§7 表格已按 6K LUT 估

#### 4.2.3 `key_to_fp`（嵌在 `output_buf` 写通道前）

完全组合逻辑，每 lane 独立。与软件 `KeyToFloatVFImpl` 一致：

```
key_to_fp:
    if (k[31] == 1)       return k ^ 0x80000000;      // 原本是正数 / +0 / +Inf / NaN
    else                  return ~k;                  // 原本是负数 / -0 / -Inf
```

**与 `fp_to_key` 往返的语义**：
- **有限数** (`±normal / ±subnormal / ±0 / ±Inf`)：**bit-exact 完美还原**。
- **NaN**：往返不保证原 bit pattern 一致：
  - canonical qNaN `0x7FC00000` 在正变换时被强制压到 `0xFFFFFFFF`，反向得到 `0x7FFFFFFF`（一个 +NaN，bit pattern 改变但仍满足 `exp=0xFF, mantissa≠0`）。
  - 其他 NaN（sNaN / 其他 payload）在正反变换中只走 XOR 通路，反向之后仍是 NaN，但 payload 可能与原值不同。
  - 即"**是 NaN**"的性质守恒，但具体 bit pattern 不保证。下游若依赖某种特定 NaN 表示（例如把 NaN 用作 sentinel），需要在 `output_buf` 之后自行 canonicalize。

每 lane ~5 LUT，128 lanes ≈ 0.6K LUT。位置选择在 `filter_compact` 写入 `output_buf` 的链路上，这样下游每次发出时不再需要变换。

#### 4.2.4 `fp_to_key`（嵌在 `data_sram` 写口前）

```
fp_to_key:
    if (x == 0x7FC00000)  return 0xFFFFFFFF;          // canonical qNaN -> 顶
    if (x[31] == 0)       return x ^ 0x80000000;      // 正数 / +0 / +Inf / +subnormal
    else                  return ~x;                  // 负数 / -0 / -Inf / -subnormal / -NaN
```

**IEEE 754 特殊值处理**：

| 输入 | 显式分支命中？ | 经过运算 | 输出 key | 在排序空间的位置 |
|---|:---:|---|---|---|
| `+normal / +subnormal` | 否 | `x ^ 0x80000000` | `0x80000001 ~ 0xFF7FFFFF` | 正数区，越大越上 |
| `+0` (`0x00000000`) | 否 | XOR `0x80000000` | `0x80000000` | 正数区最小 |
| `-0` (`0x80000000`) | 否（sign=1 走 `~x`）| `~x` | `0x7FFFFFFF` | 负数区最大 → **-0 < +0** |
| `-normal / -subnormal` | 否 | `~x` | `0x00800000 ~ 0x7FFFFFFE` | 负数区 |
| `+Inf` (`0x7F800000`) | 否 | XOR `0x80000000` | `0xFF800000` | 正数区最上方 |
| `-Inf` (`0xFF800000`) | 否 | `~x` | `0x007FFFFF` | 负数区最下方 |
| canonical qNaN (`0x7FC00000`) | **是** | 强制 `0xFFFFFFFF` | `0xFFFFFFFF` | 排序空间绝对顶部 |
| 其他 NaN（sNaN / 非 canonical qNaN）| 否 | XOR 通路 | `0xFF800001 ~ 0xFFFFFFFF`（含 `0xFFFFFFFF`，与 canonical 撞顶）或 `0x00000000 ~ 0x007FFFFE` | 在 +Inf 之上 / -Inf 之下，仍然排在所有有限数外侧 |

**要点**：
- **±Inf / ±0 / subnormal 不需要显式分支**，标准 XOR 已经把 IEEE 754 bit pattern 字典序对齐到数值序，自动落到正确位置（这是 sortable key 算法的核心性质）。
- **NaN 的"是 NaN"性质会被守恒**：所有 NaN 都有 `exp = 0xFF, mantissa ≠ 0`，XOR 之后仍满足这个条件，反向也仍是 NaN。canonical qNaN 这一支唯一的作用是把它压到 `0xFFFFFFFF` 的"标准顶位"，方便和其他 NaN 区分（实际下游不依赖这点）。
- **如果上游能保证 NaN 不会出现**（典型场景：score 来自 softmax / sigmoid / sqrt 之后的有效张量），第一行的 `== 0x7FC00000` 分支可以**整段去掉**，节省 128 lane × 32-bit 等值比较器 ≈ **0.5K LUT**，并把 `fp_to_key` 的关键路径减短 1 级。

每 lane ~10 LUT（带 NaN 分支）或 ~5 LUT（去掉 NaN 分支），128 lanes ≈ 0.6 ~ 1.3K LUT。位置选择在 `in_data` → `data_sram.wdata` 之间的组合通路上（详见 §4.2.1）。

#### 4.2.5 `histogram_engine`（关键路径模块）

输入：每周期 128 × (8-bit byte, 1-bit valid)
输出：256 × 11-bit 直方图（在 8 cycle 末尾稳定）

**架构：one-hot 解码 → 列向 popcount → 8 周期累加**

```
                  lane[0]   lane[1]   ...   lane[127]
                    │         │              │
                    ▼         ▼              ▼
           ┌─────────────────────────────────────┐
           │   8b → 256b One-Hot Decoder × 128   │
           └─────────────────────────────────────┘
             │  256 lines × 128 bits each
             ▼
           ┌─────────────────────────────────────┐
           │   Column-wise Popcount × 256        │
           │   (each: 128-input → 8-bit output)  │
           └─────────────────────────────────────┘
                    │   256 × 8 bit
                    ▼
           ┌─────────────────────────────────────┐
           │   256 × 11-bit Accumulators         │
           │   (累加 8 cycle, 在 init 周期清零)  │
           └─────────────────────────────────────┘
```

资源估算（**给区间，避免过度乐观**）：

| 子结构 | LUT (下限) | LUT (上限) | 说明 |
|---|---:|---:|---|
| 128 × (8→256) one-hot decoder | ~8K | ~16K | 每 lane 约 64~128 LUT6（按 LUT6 4 输出/组估算）|
| 256 × 128-input popcount tree | ~20K | ~30K | 单 tree CSA + 加法树，难以完全共享 |
| 256 × 11-bit accumulator | ~3K | ~5K | 256 × 11-bit adder + FF |
| valid gating + 走线开销 | ~2K | ~5K | mask AND + 路由膨胀 |
| **总计** | **~35K LUT** | **~55K LUT** | + ~5K FF |

> 原估"~30K LUT"假设了高度共享 decoder + 紧凑 popcount，**实际综合（含 PnR 膨胀）更可能落在 40~50K LUT**。§7 资源表已按上限 50K LUT 取保守值。

可选优化：先把 128 lanes 分 8 组（每组 16 lanes），每组先做小 popcount（16→5 bit，每个 ~12 LUT），最终用一个 5 级加法树合并 → 减少 LUT 集中度，但总 LUT 不一定降低，主要改善 routing congestion。

#### 4.2.6 `cumsum_threshold`

输入：256 × 11-bit 直方图 `hist[0..255]`，当前 `bottomK_R`
输出：`target_bin_R`（8-bit），`prev_cum_R`（11-bit，用于下一轮 bottomK 更新）

实现：
1. **并行前缀和** （Brent-Kung 或 Kogge-Stone，9 级，对 256 路 11-bit）
2. **阈值比较** ：找到最小 `j` 使 `cumsum[j] ≥ bottomK_R` → priority encoder
3. **更新**：
   ```
   target_bin_R = j
   prev_cum_R   = (j == 0) ? 0 : cumsum[j-1]
   bottomK_{R+1} = bottomK_R - prev_cum_R
   ```

延迟：**严格 4 cycle（硬上限）**

| stage | 内容 | 备注 |
|---|---|---|
| cy 0 | prefix-scan 前半（Kogge-Stone level 0~3） | 4 级加法塞 1 cycle |
| cy 1 | prefix-scan 后半（level 4~7） | 同上 |
| cy 2 | priority encode（最小 `j` s.t. `cumsum[j] ≥ bottomK_R`） | 256→8 PE |
| cy 3 | 减法 `bottomK_{R+1} = bottomK_R - prev_cum_R` + 寄存 | |

> **整个 RADIX 节拍 12 cy = 8 cy hist + 4 cy cumsum 没有任何余量**：
> - cumsum 是 **频率收敛的最关键路径候选**，必须在 STA 中单独签出
> - 若 500 MHz 下 prefix-scan "4 级/cycle" 收不下来，**只能选**：(a) 把每个 RADIX round 拉长（破坏节拍），(b) 降频，(c) 改用更小 radix（如 16-bin × 8 轮，prefix-scan 5 级）
> - §9 验证策略已把它列为必跑项

寄存器：cumsum_threshold 内部需要锁存

- `target_bin[0..3]` ：4 × 8-bit = 32 bit FF（送 `kth_compose`）
- `bottomK` ：11-bit FF（跨轮更新）
- `prev_cum` ：11-bit FF（中间量）

资源：256 × 11-bit prefix scan ≈ ~6K LUT + ~3K FF + 上面的小寄存器 ≈ 54 bit

#### 4.2.7 `mask_reg` + `mask_update`

- **`mask_reg`**：1024-bit 寄存器，初始 = `{1024{1'b1}}`
- **`mask_update`**：每周期处理 128 lane

```
new_mask[lane] = old_mask[lane]
              & (current_byte[lane] == target_bin_R);

mask_reg[addr*128 + lane] <= new_mask[lane];
```

每周期 128 个 8-bit 比较器 + 128 个 AND，分 8 cycle 完成全部 1024 位的更新。

**与下一轮 histogram 融合**：在同一周期，`mask_update` 输出的 `new_mask[lane]` 直接作为 `histogram_engine` 当前周期的 `valid` 信号 → 节省 8 cycle/round（详见 §5）。

#### 4.2.8 `kth_compose`

```verilog
assign kth_key = {idx0[7:0], idx1[7:0], idx2[7:0], idx3[7:0]};
```

纯线连接，0 cycle 延迟。

#### 4.2.9 `filter_compact`

输入：每周期 128 × 32-bit key（来自 `data_sram`），`kth_key`（来自 `kth_compose`），`cfg_topk_eff`
输出：紧凑写入 `output_buf.{value_ram, index_ram}`

**元素原始位置 index 的来源**（重要）：

```
elem_idx[lane] = sram_raddr * LANE_NUM + lane     // = sram_raddr * 128 + lane
                                                  // 实际只 10 bit 有效 (N=1024)
```

**不存任何 SRAM**，是从扫描位置纯组合派生。在写入 `output_buf.index_ram` 时高位补 0 凑足 32-bit。

**单遍设计**（v1 实现选择，比软件版的两遍流程少 8 cy）：

GT / EQ 谓词在同一 cycle 内并行求值，EQ 通过一个上一阶段就算好的
`eq_keep` 计数器做 lane-级截断。这样 FILTER 段一次性扫完 `data_sram`，
当 `wptr_next == K`（或扫到第 8 拍）就退出，单遍 1..8 cy。

**eq_keep 推导**（关键点：histograms 已经隐含 GT 计数）：

  ```
  bottomK_4 = bottomK_3 − prev_cum_3            (round 3 末次更新后的 bottomK)
  eq_keep   = hist_3[target_bin_3] − bottomK_4 + 1
  gt_count  = K − eq_keep
  ```

直觉：round 3 把元素压到"全 4 字节都等于 target_bin"的小集合（其大小
= `hist_3[tb_3]`），其中前 `bottomK_4 − 1` 个（扫描序）排在 K 名之外，
第 `bottomK_4` 个起就是 top-K 的尾部。

**实现**：在 CUMSUM round 3 那一拍并行算出 `hist_3[tb_3]`（256→1 mux）+
减法 + 1，落到 `eq_remain` 寄存器；下一拍 FILTER 开始时直接用。

**每 cycle 流水**（扫描 `data_sram` beat `t = 0..7`）：

```
# 谓词（128 lane 并行）
pred_gt[lane]  = (sram_key[lane] >  kth_key)
pred_eq[lane]  = (sram_key[lane] == kth_key)        # gt 和 eq 天然互斥

# EQ lane-级 gating: 留下扫描序里前 eq_remain 个 EQ
eq_pos[lane]   = popcount(pred_eq[0..lane])         # 128 路 8-bit prefix-sum (#1)
eq_kept[lane]  = pred_eq[lane] & (eq_pos[lane] <= eq_remain)
eq_taken_cy    = min(eq_total, eq_remain)           # 一对一减法 + min

# 合并谓词 + compaction
pred[lane]     = pred_gt[lane] | eq_kept[lane]
wpos[lane]     = popcount(pred[0..lane])            # 128 路 8-bit prefix-sum (#2)
# value 经 key_to_fp 还原后, 与 elem_idx 一起进 output_buf
write_lane_to_output_buf(pred, wpos, wptr, key_to_fp(sram_key), elem_idx)

# 状态更新 + 早停
eq_remain <= eq_remain - eq_taken_cy
wptr      <= wptr + popcount(pred)
if (wptr_next == K) or (t == 7):  FILTER -> WAIT_OUT
```

**Ties 选择规则**（与软件 `vf_topk.h` bit-exact 对齐用）：

EQ pass 选中等值元素的顺序 = `data_sram` 扫描序，即 `(addr ascending, lane ascending)`。
合并到单遍后这条约定不变（`eq_pos` 的 prefix-sum 自然按 lane 升序排列）。

资源：
- 128 × 32-bit comparator (GT) + 128 × 32-bit comparator (EQ) ≈ ~4K LUT（GT/EQ 比较器架构可部分共享，仅末级谓词不同）
- 2 × 128-lane 8-bit prefix scan + min 比较器 + `eq_remain` 减法 ≈ ~3K LUT
- 256→1 mux on 11-bit hist 取 `hist_3[tb_3]`（仅在 CUMSUM_R3 那拍用一次）≈ ~0.5K LUT
- 紧凑写入的 barrel-shifter / per-bank we 已计入 `output_buf`（§4.2.2）

**与"两遍"原方案的代价对比**：

| 项 | 两遍方案 | 单遍方案（本实现）|
|---|---|---|
| 平均 FILTER cy | 8 + 0..8 cy | 1..8 cy（小 K 显著更快）|
| 端到端延迟 K=128 | 82 cy 最坏 | 74 cy 最坏 |
| 端到端延迟 K=900 | 82 cy | 74 cy |
| 每 cy LUT | 单 prefix-sum（GT）+ 双 prefix-sum（EQ）| 双 prefix-sum + min |
| 额外组合 | — | 1 × 256→1 mux on hist（只在 CUMSUM_R3 路径上）|
| FSM state 数 | 2（FILT_GT, FILT_EQ）| 1（FILTER）|

#### 4.2.10 `topk_ctrl` FSM

`topk_ctrl` 内含两层：

1. **recv 子机**（写口侧）：监听 `in_req`，跑 8-cycle 写指针把 `in_data` 经 `fp_to_key` 直写 `data_sram`。完成后拉高 `load_done`。
2. **主 FSM**（计算侧）：被 `load_done` 启动，依次走 RADIX × 4 → KTH_COMPOSE → 单遍 FILTER，然后把 `filter_done` 交给 `output_buf` 的 send 子机自驱动发出 `out_req`。

```
                    ┌──────┐
                    │ IDLE │◄──────────────────────┐
                    └──┬───┘                       │
                       │ load_done (来自 recv 子机) │
                       ▼                           │
                ┌─────────────┐                    │
                │  RADIX_R0   │ 17 cycle (HIST8+CUM1+MASK8)
                └──────┬──────┘                    │
                       ▼                           │
                ┌─────────────┐                    │
                │  RADIX_R1   │ 17 cycle           │
                └──────┬──────┘                    │
                       ▼                           │
                ┌─────────────┐                    │
                │  RADIX_R2   │ 17 cycle           │
                └──────┬──────┘                    │
                       ▼                           │
                ┌─────────────┐                    │
                │  RADIX_R3   │ 9 cycle (HIST8+CUM1, eq_keep 在 CUM 末拍同周期算出)
                └──────┬──────┘                    │
                       │ kth_key 在 R3 末拍组合可见 │
                       ▼                           │
                ┌─────────────┐                    │
                │   FILTER    │ 1..8 cycle 单遍    │
                │ (GT ∪ EQ_kept)                   │
                └──────┬──────┘                    │
                       │ wptr_next == K  OR        │
                       │  at_beat_last             │
                       ▼                           │
                  filter_done = 1                  │
                  (output_buf 接管发送)            │
                       │                           │
                       └─── output_buf send_done ──┘
                            (status_busy ← 0)
```

两个写口侧 / 发送侧的并行子机：

```
data_sram 写口 recv 子机:          output_buf send 子机:
  IDLE ──in_req──► RECV            IDLE ──filter_done──► ISSUE
  RECV ──cnt==7──► IDLE                                  ISSUE ──► SEND
       (load_done = 1 last cy)     SEND ──cnt==7──► IDLE
                                        (send_done = 1 last cy)
```

> recv 子机和主 FSM 解耦的好处：未来开 ping-pong 时（§11），recv 子机可以在主 FSM 还在跑 RADIX 时就把下一帧灌进备份的 `data_sram_B`，不需要等当前计算完成。

---

## 5. 执行流程详解

### 5.1 关键的「mask 更新 + 下一轮 histogram」融合

```
                  ┌──────────────────┐
                  │  data_sram 读出  │ 1 word/lane/cycle
                  └────────┬─────────┘
                           │ 128 × 32 bit
            ┌──────────────┼──────────────┐
            ▼              ▼              ▼
      byte_select_R   byte_select_{R+1}  pass-through
            │              │
            ▼              │
     == target_bin_R       │
            │              │
            ▼              │
     AND old_mask[lane]    │
            │  └─► new_mask[lane] (registered)
            │              │
            └──────┐  ┌────┘
                   ▼  ▼
              AND (gating)
                   │
                   ▼
            histogram_engine_{R+1}
            (该周期使用刚算出来的 new_mask)
```

这样 round R 的 mask 更新和 round R+1 的 histogram 共享同一次 SRAM 读 → 一个 round = 12 cycle（8 cy SRAM + 4 cy cumsum 流水）。

### 5.2 完整时序（以 `in_req` 拉高拍为 t=0）

v1 RTL 把 CUMSUM 实现为单 cy 组合通路（没有 4 cy 的内部流水寄存器），
并把 MASK 单独跑 8 cy 而不是和下一轮 HIST 融合，所以每轮 RADIX = HIST 8
+ CUMSUM 1 + MASK 8 = 17 cy（最后一轮无 MASK = 9 cy）。FILTER 改为单遍
1..8 cy。kth_key 由 target_bin_lat 寄存器组合派生，不再单独占 KTH cycle。

| 周期 | 阶段 | 主要活动 | 接口动作 |
|:--:|:--:|:--|:--|
| 0 | REQ_RX | 上游拉高 in_req | recv 子机进入 RECV |
| 1–8 | LOAD | recv 子机驱动 `data_sram` 写口：8 beat × 4096 bit = 1024 fp32 经 `fp_to_key` 直写 `data_sram` | in_data 必须连续有效 |
| 9–16 | RADIX_R0 HIST | 读 data_sram, **byte_R0 = key[31:24] (MSB)** 累加进 hist_0, mask = all 1s | — |
| 17 | RADIX_R0 CUMSUM | hist_0 → cumsum + priority encode → target_bin_lat[0] | — |
| 18–25 | RADIX_R0 MASK | 用 target_bin_lat[0] 收紧 mask_reg（128 lane × 8 行）| — |
| 26–33 | RADIX_R1 HIST | **byte_R1 = key[23:16]**, mask=R0 后的结果 | — |
| 34 | RADIX_R1 CUMSUM | → target_bin_lat[1] | — |
| 35–42 | RADIX_R1 MASK | 用 target_bin_lat[1] 收紧 mask_reg | — |
| 43–50 | RADIX_R2 HIST | **byte_R2 = key[15:8]** | — |
| 51 | RADIX_R2 CUMSUM | → target_bin_lat[2] | — |
| 52–59 | RADIX_R2 MASK | 用 target_bin_lat[2] 收紧 mask_reg | — |
| 60–67 | RADIX_R3 HIST | **byte_R3 = key[7:0] (LSB)** | — |
| 68 | RADIX_R3 CUMSUM | → target_bin_lat[3]，同周期算 `eq_keep` 并写入 `eq_remain` | — |
| 69..(69+F−1) | FILTER | 单遍扫 data_sram，GT ∪ EQ_kept 同 cy 求值并紧凑写入 output_buf；F = 1..8（直到 `wptr_next == K` 或 sub_step==7）| — |
| 69+F | OUT_ISSUE | output_buf 发出 out_req (1 拍) | **out_req 拉高** |
| 70+F..77+F | DRAIN | 8 拍连续读 output_buf 输出 | **out_value/out_index_data 连续 8 拍有效**, 配 out_valid_mask |
| 78+F | DONE | status_busy ← 0 | — |

其中 F = `min(ceil(K / LANE_NUM), 8)`（K=128/256/.../1024 是边界，但 GT≠0 时 F 通常更小）。

**延迟汇总**：

| 指标 | 值 | 备注 |
|---|---|---|
| in_req → out_req（最坏，K=900 EQ 跑满）| **77 cycle** | LOAD 8 + RADIX (3×17+9=60) + FILTER 8 + ISSUE 1 |
| in_req → 末拍 DRAIN（最坏）| **85 cycle** | 上面 + 8 cy DRAIN − 1 |
| in_req → out_req（小 K，K=4 smoke）| **70 cycle** | FILTER 只跑 1 cy |
| in_req → 末拍 DRAIN（小 K）| **78 cycle** | |

> 与原"4 cy cumsum 流水 + MASK/HIST 融合"假设的延迟（74 / 82 cy）相比，
> v1 RTL 因为不流水 cumsum + 不融合 mask 多了几个 cy；但单遍 FILTER 又
> 把最坏 case 砍掉 8 cy，最终最坏 85 cy ≈ 旧"两遍"方案的最坏 90 cy（旧
> 84 cy 双 FILTER + 1 + 8 = 93 cy），仍在原 §1 给的 65~90 cy 范围内。

### 5.3 流水化（ping-pong，§11 详述）

如果用户希望持续接受新的 `in_req`，可启用 ping-pong：

- 实例化两份 `data_sram`（A/B）和两份 `mask_reg`
- recv 子机每接到一次 `in_req` 就交替写入 A 或 B
- 内部计算阶段在 A/B 之间切换
- `output_buf` 同样可以做 ping-pong

启用后：
- 单任务端到端延迟不变（70 ~ 85 cycle, 见 §5.2）
- 吞吐由 COMPUTE 段（RADIX 60 + 单遍 FILTER 1..8 = **61 ~ 68 cy**）决定：
  - 典型 K（小 K，FILTER 1 cy）：**1 任务 / 61 cy**
  - 最坏 K（FILTER 跑满 8 cy）：**1 任务 / 68 cy**
  - LOAD (8 cy) 与 DRAIN (8 cy) 在 ping-pong 下可与 COMPUTE 完全重叠，不再是瓶颈
- 资源代价：`data_sram` + `mask_reg` + `output_buf` 翻倍（详见 §7 ping-pong 增量）

---

## 6. 数据通路与控制信号

### 6.1 关键内部信号

**命名约定（重要）**：
- **元素原始位置 index**：`elem_idx[lane]` / `obuf_wdata_index`，每 lane 32 bit（仅低 ⌈log2 N⌉=10 bit 有效），由扫描位置 `sram_raddr*128 + lane` 派生，不存 SRAM
- 本设计不再维护"任务/帧 ID"信号；上游若需配对请按 §3.3 自行处理

| 信号 | 位宽 | 来源 → 去向 | 说明 |
|---|---:|---|---|
| `load_done` | 1 | recv 子机 → 主 FSM | recv 子机收完 8 beat 拉高 1 拍 |
| `sram_waddr` | 3 | recv 子机 → data_sram | LOAD 阶段写地址 (0..7) |
| `sram_wdata` | 4096 | fp_to_key (组合) → data_sram | 已变换为 sortable key |
| `sram_we`    | 1 | recv 子机 → data_sram | LOAD 阶段每拍高一次 |
| `sram_raddr` | 3 | topk_ctrl → data_sram | RADIX/FILTER 读地址 (0..7) |
| `sram_rdata` | 4096 | data_sram → byte_select / filter | 一行 128 lanes |
| `cur_round`  | 2 | topk_ctrl → byte_select | 0..3 = byte 31:24, 23:16, 15:8, 7:0 (MSB-first) |
| `target_bin[R]` | 8 | cumsum → mask_update / kth_compose | 第 R 轮命中的 bin |
| `target_bin_lat[0..3]` | 4×8=32 (FF) | cumsum → kth_compose | 4 轮 target_bin 锁存，组合拼出 `kth_key` |
| `bottomK`    | 11 (FF) | topk_ctrl → cumsum / eq_keep | 起始 = N − K + 1，每轮 `bottomK -= prev_cum`；R3 末拍同 cy 用于算 `eq_keep` |
| `prev_cum`   | 11 (FF) | cumsum 内部 | 当前轮的 `cumsum[j-1]`，用于更新 bottomK |
| `mask_word`  | 128 | mask_reg → mask_update / hist | 当前行 mask |
| `mask_reg`   | 1024 (FF) | mask_update → next round | 全局活跃元素 mask, 初始全 1 |
| `hist_bins`  | 256×11 | hist → cumsum / eq_keep | 完整直方图；CUMSUM_R3 末拍同 cy 取 `hist[target_bin_3]` 算 eq_keep |
| `kth_key`    | 32 | kth_compose → filter | 拼装后的第 K 大值（仍是 sortable key 形态） |
| `eq_remain`  | 11 (FF) | CUMSUM_R3 末拍锁存 `eq_keep` → FILTER | 还要写的 EQ 元素数；每 FILTER cy 减 `min(eq_total, eq_remain)` |
| `total_count` | 11 (FF) | filter → output_buf | 整个 FILTER 写出的元素总数（= K），用于生成 out_valid_mask |
| `filter_done` | 1 | filter → output_buf | `wptr_next == K` 或 `at_beat_last`，触发 output_buf 发出 out_req |
| `obuf_waddr` | 11 | filter → output_buf | 紧凑写指针 `wptr` |
| `obuf_wdata_value` | 4096 | key_to_fp → output_buf | 已还原为 fp32 |
| `obuf_wdata_index` | 4096 | filter (`elem_idx = sram_raddr*128 + lane`, 高位补 0) → output_buf | **元素原始位置 index**, 每 lane 32-bit（低 10 bit 有效） |
| `send_done` | 1 | output_buf → topk_ctrl | DRAIN 完成, status_busy 拉低 |

### 6.2 SRAM 端口

| SRAM | 写口 | 读口 | 备注 |
|---|---|---|---|
| `data_sram` | 4096 b @ LOAD 阶段（recv 子机独占，前置 fp_to_key 组合） | 4096 b @ RADIX + FILTER 阶段 | **推荐 LUTRAM/FF，BRAM 浪费**（详见 §4.2.1） |
| `output_buf.value_ram` | 4096 b @ FILTER 阶段（紧凑写, 部分 lane 写 + barrel shifter） | 4096 b @ DRAIN 阶段 | 同上；写口需 per-bank we |
| `output_buf.index_ram` | 同上 | 同上 | 同上 |

由于写口和读口在时间上不重叠（同一个任务内），用 simple dual-port (1R1W) 即可。**实现介质上推荐 LUTRAM（128 bank × 8 deep 太浅，BRAM 利用率 < 1%）**，详见 §4.2.1 / §7。

### 6.3 接口握手详细波形

```wavedrom
{ "signal": [
  { "name": "clk",            "wave": "p........|p........" },
  {},
  ["input",
    { "name": "in_req",       "wave": "010......|........." },
    { "name": "in_data",      "wave": "x.=======|.........", "data": ["D0","D1","D2","D3","D4","D5","D6","D7"] }
  ],
  {},
  { "name": "status_busy",    "wave": "01.......|.......10" },
  {},
  ["output",
    { "name": "out_req",        "wave": "0........|10......." },
    { "name": "out_value",      "wave": "x........|x========", "data": ["D0","D1","D2","D3","D4","D5","D6","D7"] },
    { "name": "out_index_data", "wave": "x........|x========", "data": ["D0","D1","D2","D3","D4","D5","D6","D7"] },
    { "name": "out_valid_mask", "wave": "x........|x=======3", "data": ["all 1","all 1","all 1","all 1","all 1","all 1","all 1","low 4"] }
  ]
],
  "head": { "text": "in_req (cy 0) → 内部计算 ~74 cycle → out_req → 8-cycle DRAIN burst (示例 K=900, 末拍 low 4)" },
  "foot": { "text": "status_busy 在 in_req 采样后置 1, DRAIN 末拍后清 0; 中间 '|' 处省略约 65 cycle 内部计算 (RADIX×4 + FILTER GT/EQ)" }
}
```

---

## 7. 资源估算（仅参考，实际综合后会有偏差）

**资源估算原则**：取保守值，避免后续 PnR 才发现欠估。`data_sram` / `output_buf.{value,index}_ram` 推荐用 LUTRAM 实现（详见 §4.2.1），所以也在 LUT 列计入存储开销；并行给出 "若改用 BRAM" 的替代映射。

| 模块 | LUT | FF | BRAM/LUTRAM | 备注 |
|---|---:|---:|---:|---|
| `data_sram` (storage, LUTRAM 实现) | ~4K | – | 128×8×32b LUTRAM | 或 32K FF；不推荐 1~2 BRAM36K（利用率<1%）|
| `data_sram` 写口 shim (recv 子机 + waddr counter) | 0.1K | 4 | – | 3-bit counter |
| `fp_to_key` × 128 | 1.3K | – | – | 嵌在 `data_sram` 写口前的组合逻辑 |
| `byte_select` × 128 | 0.5K | – | – | 4-to-1 byte mux |
| `mask_reg` | – | 1024 | – | 1024-bit FF |
| `mask_update` | 0.5K | – | – | 128 lane 比较 + AND |
| `histogram_engine` | **~50K** | 5K | – | one-hot + 列 popcount + 累加；区间 35~55K，取上限 |
| `cumsum_threshold` (含 target_bin[0..3]/bottomK/prev_cum 锁存) | 6K | 3K + ~54 | – | 256-elem 并行前缀和，硬上限 4 cy（§4.2.6）|
| `kth_compose` | – | 32 | – | 纯线连接 |
| `filter_compact` (GT/EQ 比较 + 双 prefix-scan + EQ 截断 + hist[tb] mux) | 7.5K | 1K | – | 见 §4.2.9；单遍方案多一个 256→1 mux on 11-bit (~0.5K LUT)，但少一个 FSM state |
| `key_to_fp` × 128 | 0.6K | – | – | 嵌在 filter→output_buf 之间 |
| `output_buf` (storage, LUTRAM 实现) | ~8K | – | 2×128×8×32b LUTRAM | 或 64K FF；不推荐 BRAM |
| `output_buf` 紧凑写入逻辑 (prefix-sum + barrel shifter + per-bank we, 见 §4.2.2) | **~6K** | – | – | 原 1.5K LUT 严重低估 |
| `output_buf` send 子机 + valid_mask 生成器 | 0.5K | 100 | – | – |
| `topk_ctrl` 主 FSM | 0.5K | 0.5K | – | 顶层调度 |
| **总计（单实例）** | **~85K LUT** | **~11K FF** | **96 Kb LUTRAM** (或同等 FF) | 较旧估 48K LUT 上调约 1.8× |
| **+ Ping-pong 双 buffer 增量** | **+1.5K** | **+34K** | **+96 Kb LUTRAM** | data_sram + output_buf 翻倍 (+12K LUT 存储) + mask_reg×2 (+1K FF) + bank-select mux (~0.7K LUT) + 控制 (~0.3K LUT)；若用 LUTRAM, FF 增量小；若用 FF 实现则 FF 增量为 +64K |

> 单实例总量比原版 48K LUT 上调到 ~85K LUT，主要来自 (a) histogram 取上限、(b) output_buf 紧凑写入补足。定位仍是中等规模 FPGA（Xilinx ZU7EV / Intel Agilex Mid-Range）可容纳，但已不能称"轻松"；若 LUT 紧张，可先考虑 §11 的"更高 radix"或 `RADIX_BITS=4` trade-off。

---

## 8. 配置 / 参数化扩展

下表列出可参数化的设计点，便于后续从 N=1024 扩展：

| 参数 | 当前值 | 扩展含义 | 影响 |
|---|---|---|---|
| `N` | 1024 | 输入元素数 | SRAM 深度、bottomK 位宽、histogram 累加器位宽 |
| `LANE_NUM` | 128 | 并行通道数 | histogram 解码器、popcount、mask 比特数 |
| `RADIX_BITS` | 8 | 每轮 radix 位数 | bin 数 = 2^RADIX_BITS；轮数 = 32/RADIX_BITS |
| `K_MAX` | N | 最大 K | 输出 SRAM 大小 |
| `DTYPE` | fp32 | 数据格式 | bf16/fp16 → 减少 radix 轮数；定点 → 跳过 fp_to_key |

把 `RADIX_BITS=4` 可以减少 histogram 资源（16 bin）但轮数翻倍至 8 轮，是经典的「面积 ↔ 延迟」trade-off。

---

## 9. 验证策略建议

1. **单元仿真**：
   - `fp_to_key` / `key_to_fp`：穷举一组 corner（NaN、±0、±∞、subnormal、normal+/-）；专门检查 NaN 往返的"是 NaN"性质守恒（payload 可改变）
   - `histogram_engine`：随机 8 cycle 输入，对比软件参考模型
   - `cumsum_threshold`：随机直方图 + 随机 bottomK，检查 target_bin 与累加正确性
   - `filter_compact`：
     - 随机谓词，检查 prefix scan + 紧凑写入
     - **EQ 截断专项**：构造 `remaining` 小于本周期 EQ 命中数的场景，确认 lane-级 gating 正确（不会写超 K）
     - **Ties 顺序专项**：所有元素相等 / 多段连续相等，确认按 `(addr, lane)` 扫描序选取，与软件 reference 一致

2. **集成仿真**：
   - 1024 个随机 fp32（含负数、NaN、±0）输入
   - 与软件版 `vf_topk.h`（IsFloat=true）做 **bit-exact 对比**；ties 选择遵循"扫描序"约定（§4.2.9）
   - 边界：K=1, K=N, K=0 (硬件夹紧到 1), 全相同值, 全 NaN, 输出最后一拍部分 lane (K mod 128 ≠ 0)

3. **形式验证**（可选）：
   - mask_reg 单调收紧性质（每轮 1 的个数 ≤ 上一轮）
   - bottomK 恒等性质（bottomK_R = N − target_count_above_threshold）

4. **流片前 PPA**（必跑项）：
   - 综合到目标频率 500 MHz，**`cumsum_threshold` 256-elem 11-bit prefix scan 是 #1 关键路径候选**（4 cy 硬上限，无余量，详见 §4.2.6）；若收不下来必须降频或改 RADIX_BITS=4
   - `output_buf` 的 128-way barrel shifter (§4.2.2) 是 #2 候选
   - `histogram_engine` 256×128 popcount tree 的 routing congestion 是 #3 候选

---

## 10. 与软件版本的对应关系

| 软件函数（`vf_topk.h`） | FPGA 模块 |
|---|---|
| `FloatToKeyVFImpl` | `fp_to_key` （组合） |
| `KeyToFloatVFImpl` | `key_to_fp` （组合） |
| `HistogramsFirstVFImpl` ~ `HistogramsLastVFImpl` | `histogram_engine` × 4 次复用 |
| `FindFirstTargetBinVFImpl` ~ `FindThirdTargetBinVFImpl` | `cumsum_threshold` × 复用 |
| `FindKthVFImpl` | `kth_compose` |
| `FindValueGTOutputVFImpl` / `FindValueEQOutputVFImpl` | `filter_compact` Pass1/Pass2 |
| `FindIdxGTOutputVFImpl` / `FindIdxEQOutputVFImpl` | 同上（idx 由 `sram_raddr*128 + lane` 派生，不从软件 reference 的索引数组取） |
| `LiTopKVF` 顶层编排 | `topk_ctrl` FSM |

**与软件的硬约定**（必须在 bit-exact 比对的 reference 端同样实现）：

| 行为 | 硬件实现 | 软件 reference 应匹配的实现 |
|---|---|---|
| 输入元素顺序 | LOAD 阶段按 burst beat × lane 顺序写入 `data_sram` | 按相同顺序排列输入数组 |
| 元素原始位置 index | `elem_idx = sram_raddr * 128 + lane`（仅低 10 bit） | 用 `[0, N)` 自然顺序，与上式一致 |
| Ties 选择顺序 | EQ pass 按 `(sram_raddr ascending, lane ascending)` 取前 `remaining` 个 | 按相同顺序遍历输入并取等值元素 |
| K=0 行为 | 硬件夹紧 `K ← 1` | 同样行为或调用方避免 |
| NaN 输出 bit pattern | 经 `fp_to_key`/`key_to_fp` 往返，payload 可能改变 | 不依赖具体 bit pattern，仅检查 `isnan()` 性质 |
| 输出顺序 | `(sram_raddr ascending, lane within prefix-sum ascending)`，不按 value 排序 | reference 不要按 value 排序后比对，要按相同扫描序 |

---

## 11. 后续优化方向

1. **流水化多任务**：用双口 SRAM 或 ping-pong buffer，让前后两个 Top-K 任务交叠
2. **更高 radix**（16 bin × 4 轮 vs. 256 bin × 4 轮）：用 LUT 换 radix 位宽降低 cumsum 复杂度
3. **Approximate Top-K**：跳过 EQ 补齐阶段，接受 ±1 个元素的误差换更短延迟
4. **批处理**：N 改为 batch × 1024，多组数据共享 histogram 引擎
5. **片上 fp32 → bf16/fp16 截断**：当下游不需要全精度时，提前压缩可减半内部带宽

---

## 12. 文件参考

- 软件实现：`vf_topk.h`
- 软件流程文档：`vf_topk_flow.md`
- fp32 sortable key 算法来源：`quant_lightning_indexer_vector1.h::FloatToSortableKey`
