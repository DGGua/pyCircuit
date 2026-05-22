# Davinci 超标量处理器：架构总览（中文）

> 本文档面向 `designs/outerCube/davinci/` 这套 4-wide 乱序超标量处理器
> 的整理。目标是把"顶层文档 / 底层 RTL"、以及内部出现的
> **PyCircuit (PYC)、PTO ISA、Davinci 硬件 ISA** 三层关系讲清楚，
> 并交代清楚这颗核执行的指令位宽、格式以及 TREG（Tile 寄存器）
> 的格式与用法。
>
> 配套阅读：
> - 顶层架构：`designs/outerCube/Davinci_supersclar.md`（硬件 ISA + 流水线）
> - Cube 引擎：`designs/outerCube/outerCube.md`
> - 寄存器堆：`designs/outerCube/tregfile4k_v2.md`
> - 软件 ISA：`designs/outerCube/PTOISA/PTOISA.md`
> - 模块映射：`davinci/docs/MODULE_MAP.md`
> - 特性列表：`davinci/docs/FEATURE_LIST.md`

---

## 1. 三层关系：PYC / PTO / Davinci ISA

仓库里同时出现 "PY"、"PT"、"ISA" 这三个术语，指的不是同一层东西，
而是分属 **实现框架层 / 软件 ISA 层 / 硬件 ISA 层** 三个不同抽象层级：

```
   ┌──────────────────────────────────────────────────────────────────┐
   │ 软件 / 算子层                                                     │
   │   PTO ISA  (PTOISA/, "PT")                                        │
   │   - C++ intrinsic API: include/pto/common/pto_instr.hpp           │
   │   - ~150+ Tile 级算子: TADD/TMATMUL/TLOAD/TSTORE/TSORT32/...     │
   │   - 面向编译器与算子开发者, 与具体硬件解耦                        │
   └──────────────────────────────────────────────────────────────────┘
                              │  编译器 lower
                              ▼
   ┌──────────────────────────────────────────────────────────────────┐
   │ 硬件 ISA 层                                                       │
   │   Davinci ISA  (Davinci_supersclar.md, "ISA")                     │
   │   - 32-bit 定长指令, 4 个 domain:                                 │
   │       Scalar / Vector(V*) / Cube(CUBE.*) / MTE(TILE.*)            │
   │   - 是处理器真正取指、译码、重命名、发射、执行的对象              │
   └──────────────────────────────────────────────────────────────────┘
                              │  RTL 实现
                              ▼
   ┌──────────────────────────────────────────────────────────────────┐
   │ RTL / 实现层                                                      │
   │   PyCircuit V5  ("PY" / "PYC")                                    │
   │   - pyCircuit 框架 (compiler/frontend/pycircuit), 一种 Python DSL │
   │   - 用 domain.signal / <<= / .assign / cas / mux 来写 RTL          │
   │   - davinci/ 下所有 *.py 都是这一层                                │
   └──────────────────────────────────────────────────────────────────┘
```

三者的职责差别一句话总结：

| 缩写 | 全称 | 层级 | 角色 | 你需要在哪里看 |
|------|------|------|------|----------------|
| **PT / PTO** | PTO Tile Lib ISA | 软件 ISA | 给上层算子和编译器用的 Tile 抽象 | `PTOISA/*.md`、`include/pto/common/pto_instr.hpp` |
| **ISA** | Davinci Hardware ISA | 硬件 ISA | 处理器实际识别和执行的 32-bit 指令集 | `Davinci_supersclar.md` §2 |
| **PY / PYC** | PyCircuit V5 | RTL DSL | 用来"画硬件"的 Python 框架 | `compiler/frontend/pycircuit/`，`davinci/**/*.py` |

### 1.1 PTO 与 Davinci ISA 的映射

PTO 指令是"软件视角"的算子，每条 PTO 指令在 Davinci 上会对应到
**某个硬件 domain 的一条或一组硬件指令**。`Davinci_supersclar.md` §2.2.4
给出了完整对照表，下面节选关键的几类：

| PTO（软件） | Davinci 硬件指令 | 落在哪个 domain |
|-------------|-------------------|-----------------|
| `TADD` / `TSUB` / `TMUL` / `TMAX` ... | `VADD` / `VSUB` / `VMUL` / `VMAX` ... | Vector |
| `TADDS` / `TMULS` ...（tile-标量） | `VADDS` / `VMULS` ... | Vector |
| `TROWSUM` / `TCOLMAX` ... | `VROWSUM` / `VCOLMAX` ... | Vector |
| `TMATMUL` / `TMATMUL_ACC` / `TMATMUL_BIAS` | `CUBE.OPA + CUBE.DRAIN` | Cube（外积阵列） |
| `TGEMV` / `TGEMV_MX` | `CUBE.OPA`（M=1 行） | Cube |
| `TLOAD` / `TSTORE` / `TSTORE_FP` | `TILE.LD` / `TILE.ST` | MTE |
| `MGATHER` / `MSCATTER` | `TILE.GATHER` / `TILE.SCATTER` | MTE |
| `TMOV` / `TALIAS` | `TILE.MOVE`（**纯 rename**, 0-cycle） | MTE/重命名级 |
| `TTRANS` | `TILE.TRANSPOSE` | MTE |
| `TSYNC` | `FENCE` | Scalar |
| `TASSIGN` / `TFREE` | 软件管理（Tile RAT + ref-count） | — |

> **要点**：PTO 是"概念上的指令"，Davinci 是"硬件真正执行的指令"。
> 只有 Vector / Cube / MTE 这三个 tile-domain 的硬件指令，才会真正
> 进入 Tile RAT 重命名→ 派遣到对应的 RS → 在 TRegFile-4K 上跑。

### 1.2 PyCircuit 在哪里

PyCircuit V5 是 RTL 描述语言（一种 Python eDSL），与 Davinci ISA
**没有任何语义上的关系**。`davinci/` 目录下出现的 `domain.signal`、
`<<=`、`.assign`、`cas`、`mux` 等都是 PyCircuit V5 的语法元素，
最终被编译成 MLIR / Verilog。

也就是说：

- **如果你看到 `domain.signal(width=...)` / `<<=`** → 这是 PyCircuit 语法。
- **如果你看到 `TILE.LD / VADD / CUBE.OPA`** → 这是 Davinci 硬件 ISA。
- **如果你看到 `TADD / TMATMUL / TLOAD`** → 这是 PTO 软件 ISA。

---

## 2. 顶层 / 底层模块边界

仓库里这套超标量是"**先有 spec 文档，后有 RTL**"的组织方式。
所以"顶层"和"底层"其实分两条独立的轴：**文档轴** 和 **RTL 模块轴**。

### 2.1 文档轴（spec → 实现）

```
   Davinci_supersclar.md      ← 顶层架构 spec（ISA / 流水线 / OoO 模型）
        ├── outerCube.md      ← Cube MXU 子模块 spec（4096 MAC, A/B 双模式）
        └── tregfile4k_v2.md  ← TRegFile-4K 子模块 spec（8R+8W, 1 MB）
   PTOISA/PTOISA.md           ← 平行的软件 ISA spec（与硬件解耦）

   davinci/docs/
        ├── MODULE_MAP.md     ← 顶层文档 → RTL 模块的对照
        ├── FEATURE_LIST.md   ← P0/P1 功能项的实现状态
        └── ARCHITECTURE_OVERVIEW_zh.md  ← 本文（中文总览）
```

### 2.2 RTL 模块轴（流水线方向）

`davinci/` 目录严格按"前端 → 派遣 → 后端 → 寄存器堆 + 公共件"
的流水线顺序组织。整体层次结构如下（节选自 `davinci/docs/MODULE_MAP.md`，
状态全部为 `IMPL`）：

```
davinci_top.py                              顶层结构连线
├── frontend/                               ← 顺序流水线 (F1 → D2)
│   ├── fetch/        F1-F2 PC 与 I-cache
│   ├── bpu/          分支预测（目前是简化的 bimodal）
│   ├── ibuf/         16-entry 指令队列
│   ├── decode/       D1 4-wide 解码 + domain 分类
│   └── rename/       D2 双 RAT + checkpoint
│        ├── scalar_rat.py   32 → 128
│        ├── tile_rat.py     32 → 256
│        ├── checkpoint.py   8 槽分支 checkpoint
│        └── rename.py       D2 顶层
├── dispatch/         DS：按 domain 派遣到 5 个 RS
├── backend/                                ← 乱序后端
│   ├── scalar_rs/    32-entry, 6-issue
│   ├── lsu_rs/       24-entry
│   ├── vec_rs/       16-entry
│   ├── cube_rs/      4-entry
│   ├── mte_rs/       16-entry
│   ├── scalar_exu/
│   │   ├── alu.py    4× ALU
│   │   ├── muldiv.py MUL + DIV
│   │   └── bru.py    Branch
│   ├── lsu/          标量 LD/ST + store-to-load forwarding
│   ├── vec_unit/     向量单元（16-cycle epoch 流水）
│   ├── cube_unit/    outerCube MXU 控制器
│   └── mte_unit/     Memory Tile Engine
├── regfile/                                ← 寄存器堆
│   ├── scalar_prf.py 128 × 64b, 12R+6W
│   ├── tregfile4k/   1 MB, 8R+8W tile-RF
│   └── ref_counter.py 标量 + Tile 共用引用计数
├── common/                                 ← 跨模块公共件
│   ├── parameters.py 全局参数（位宽 / 条目数 / 时延）
│   ├── cdb.py        6 端口 Common Data Bus
│   ├── tcb.py        4 端口 Tile Completion Bus
│   └── free_list.py  通用 free-list（标量 + Tile 共用）
└── tests/
    ├── unit/          每模块单元测试
    └── integration/   流水线级集成测试
```

### 2.3 顶层 davinci_top 与底层模块之间的"边界契约"

`davinci_top.py` 现在仅仅做 **结构性接线**：把 fetch / decode / rename /
dispatch / 5 个 RS / 4 个执行单元串起来，并没有做语义级的胶合。
每个子模块满足 pyCircuit V5 的"双模子模块"约定：

| 接口要素 | 约定 |
|----------|------|
| `inputs: dict \| None = None` | `None` 时是 standalone 顶层；非 None 时是被组合 |
| 返回值 | `dict[str, CycleAwareSignal \| list]` 输出信号集合 |
| `m.output(...)` | 仅在 standalone 时调用 |
| 子模块调用方式 | 父模块用 `domain.call(child, inputs={...}, prefix=...)` |

> 这是底层 RTL 的"边界纪律"：每个子模块只暴露一组命名信号，
> `davinci_top` 只做 wire 级别的 plumbing，**不做语义判断**。
> 流水线行为、时延、握手都由各个子模块内部封装。

### 2.4 跨模块全局参数

所有"硬数字"都集中在 `davinci/common/parameters.py`，避免散落在子模块里：

```text
关键参数（节选自 common/parameters.py）：
  ARCH_GREGS      = 32     X0–X31
  PHYS_GREGS      = 128    P0–P127
  SCALAR_DATA_W   = 64     标量数据通路位宽
  ARCH_TREGS      = 32     T0–T31
  PHYS_TREGS      = 256    PT0–PT255
  TILE_SIZE_BYTES = 4096   每个 Tile 4 KB
  TREGFILE_BANKS  = 64
  TREGFILE_GROUPS = 8
  TREGFILE_EPOCH_CY = 8    8-cycle 同步 calendar
  FETCH_WIDTH = DECODE_WIDTH = RENAME_WIDTH = DISPATCH_WIDTH = 4
  INSTR_WIDTH = 32         指令固定 32 bit
  CDB_PORTS   = 6          标量结果广播
  TCB_PORTS   = 4          Tile 完成广播
  CHECKPOINT_SLOTS = 8     最多 8 条在飞分支
  DOMAIN_SCALAR     = 0b00 / DOMAIN_SCALAR_ALT = 0b01
  DOMAIN_VEC_MTE    = 0b10
  DOMAIN_CUBE       = 0b11
```

新加 RTL 时**禁止在子模块里硬编码这些数字**，必须从
`common.parameters` 导入。

---

## 3. 指令位宽与格式

### 3.1 总体规则

- **所有指令一律 32-bit 定长**（`INSTR_WIDTH = 32`）。
- 取指/解码/重命名/派遣 **4 路并行**（`FETCH_WIDTH = 4`）。
- 一条 32-bit 指令的 **opcode 占低 7 位**，整体编码风格沿用 RISC-V
  的 R/I/S/U/T 五种类型（向量/Cube/MTE 复用同一套字段框架）。
- **Domain 由 `opcode[6:5]` 直接区分**，是 D1 解码与 D2 重命名/派遣的
  关键字段：

  | `opcode[6:5]` | Domain | D2 走哪条路 | 派遣到 |
  |---------------|--------|-------------|--------|
  | `00` / `01` | Scalar（含 LSU / 分支） | Scalar RAT | Scalar RS / LSU RS |
  | `10` | Vector / MTE | Tile RAT（+ Scalar RAT，若涉及标量） | Vector RS / MTE RS |
  | `11` | Cube | Tile RAT | Cube RS |

### 3.2 标量指令（Scalar ISA, RISC-V 风格）

64-bit RISC：32 个架构 GPR (X0–X31)，X0 硬连零，没有 condition flag，
分支直接比较寄存器。

```
  R-type:
   31       25 24  20 19  15 14  12 11   7 6     0
  +----------+------+------+------+------+--------+
  |  funct7  |  rs2 |  rs1 |funct3|  rd  | opcode |
  +----------+------+------+------+------+--------+

  I-type:
  +-----------------+------+------+------+--------+
  |    imm[11:0]    |  rs1 |funct3|  rd  | opcode |
  +-----------------+------+------+------+--------+
```

支持的标量指令：ALU（ADD/SUB/AND/OR/XOR/SLL/SRL/SRA/SLT/MOV）、
立即数 ALU、MUL/MULH（4-cycle 流水）、DIV/REM（12–20 cycle 非流水）、
分支（BEQ/BNE/BLT/BGE/...）、跳转（JAL/JALR）、Load（LB/LH/LW/LD/...）、
Store（SB/SH/SW/SD）、FENCE/NOP/HALT。

### 3.3 向量指令（Vector ISA）

向量指令的源/目的都是 **架构 Tile 寄存器 T0–T31**（5 bit 编码），
由 Tile RAT 重命名到物理 Tile PT0–PT255（8 bit）。

四种编码框架（`opcode = VEC`, `opcode[6:5] = 10`）：

```
  R-type (tile-tile, 例如 VADD Td, Ts1, Ts2):
   31       25 24  20 19  15 14  12 11   7 6     0
  +----------+------+------+------+------+--------+
  |  funct7  |  Ts2 |  Ts1 |funct3|  Td  | opcode |
  |   (op)   | (5b) | (5b) |(type)| (5b) |  VEC   |
  +----------+------+------+------+------+--------+

  S-type (tile-标量, 例如 VADDS Td, Ts1, Xs):
  +----------+------+------+------+------+--------+
  |  funct7  |  Xs  |  Ts1 |funct3|  Td  | opcode |
  +----------+------+------+------+------+--------+

  T-type (3-source, 例如 VFMA / VADDC):
  +----------+------+------+------+------+--------+
  |  funct7  |  Ts3 |  Ts2 |funct3|  Td  | opcode |
  +----------+------+------+------+------+--------+
       Ts1 隐式 = Td (accumulate-in-place) 或编码在 funct7 子段

  U-type (unary, 例如 VABS / VRELU):
  +----------+------+------+------+------+--------+
  |  funct7  | 00000|  Ts1 |funct3|  Td  | opcode |
  +----------+------+------+------+------+--------+
```

`funct3` 编码元素类型（FP64/FP32/FP16/BF16/FP8/INT32/INT16/INT8），
配合 `funct7` 中的 1-bit `W` 位还能扩展出 MXFP4/HiFP4 等"宽格式"。

向量 ISA 共 **95 条指令**，按用途分为：
A 逐元素算术、B Tile-标量算术、C 一元算术、D 位运算/移位、
E 比较与选择、F 类型转换、G 行规约、H 列规约、I 行广播扩展、
J 列广播扩展、K 数据搬运/permute、L Partial-tile、M 复杂多周期。

### 3.4 Cube 指令（驱动 outerCube MXU）

```
  CUBE.CFG    mode, fmt [, Mactive]   设置 A/B 模式与数据格式
  CUBE.OPA    zd, Ta, Tb, Rn          外积累加（一条指令做完整个 K 循环）
  CUBE.DRAIN  zd, Tc                  把 32-bit FP32 累加器排出到 Tile
  CUBE.ZERO   zd                      累加器清零（1 cycle）
  CUBE.WAIT   zd                      等到 pending drain 完成
```

支持格式：FP16 / BF16 / FP8 (E4M3,E5M2) / MXFP4 / HiFP4，统一累加到 FP32。
完整 spec 见 `outerCube.md` §6。

### 3.5 MTE 指令（Memory Tile Engine）

MTE 是 **三段桥**：内存 ↔ TRegFile-4K（块搬运）以及 标量 GPR ↔
TRegFile-4K 中的单元素（`TILE.GET / TILE.PUT`）。

| 指令 | 操作数 | 功能 |
|------|--------|------|
| `TILE.LD Td, [Rbase] [, Rs]` | 标量基址 + 可选步长 | 4 KB 块加载到 Td |
| `TILE.ST [Rbase], Ts [, Rs]` | 同上 | Ts 写回内存 |
| `TILE.GATHER Td, [Rbase], Tidx` | + 索引 Tile | 间接 gather |
| `TILE.SCATTER [Rbase], Ts, Tidx` | + 索引 Tile | 间接 scatter |
| `TILE.ZERO Td` | — | 清零 Tile |
| `TILE.COPY Td, Ts` | — | Tile 拷贝（分配新 PT） |
| `TILE.MOVE Td, Ts` | — | **重命名级** 别名（0-cycle） |
| `TILE.TRANSPOSE Td, Ts, fmt` | — | 经 4 KB 转置缓冲做转置 |
| `TILE.GET Rd, Ts, Ridx` | Tile + 索引 → 标量 | 单元素读，结果走 CDB |
| `TILE.PUT Td, Rs, Ridx` | 标量 + 索引 → Tile | 单元素写，RMW 语义 |

`TILE.GET` / `TILE.PUT` 的编码示例（保持与标量同一框架）：

```
  TILE.GET  Rd, Ts, Ridx:
  +----------+------+------+------+------+--------+
  |  funct7  | Ridx |  Ts  |funct3|  Rd  | opcode |
  | 0100000  | (5b) | (5b) | type | (5b) | 10xxxxx|
  +----------+------+------+------+------+--------+

  TILE.PUT  Td, Rs, Ridx:
  +----------+------+------+------+------+--------+
  |  funct7  |  Rs  | Ridx |funct3|  Td  | opcode |
  | 0100001  | (5b) | (5b) | type | (5b) | 10xxxxx|
  +----------+------+------+------+------+--------+
```

> MTE 指令在 D2 重命名时会**同时**走 Scalar RAT（基址/索引/数据）
> 和 Tile RAT（源/目的 Tile），是唯一会"双 RAT"的 domain。

---

## 4. TREG 与 TRegFile-4K：格式与用法

> 用户问的 "TREG / tell red" 实际是 **Tile Register**：
> Vector / Cube / MTE **共用同一套 Tile 寄存器堆**——
> `TRegFile-4K`，并不存在独立的"向量寄存器堆"。

### 4.1 一颗 Tile 的格式

| 字段 | 取值 | 说明 |
|------|------|------|
| 容量 | **4 KB**（4096 B） | 一个 Tile 固定 4 KB |
| 物理形状 | **64 行 × 512 bit / 行** | 64 B × 8 bank = 512 B / 行 |
| 元素布局 | 见下表 | 元素类型由指令的 `funct3` 决定 |
| 物理实体 | 256 × 4 KB = **1 MB** | 256 个物理 Tile（PT0–PT255） |

每行 512 bit 在不同元素类型下的列数：

| 元素类型 | 单元素宽度 | 列 / 行 | 行数 | 元素 / Tile | 备注 |
|----------|-----------|---------|------|-------------|------|
| FP64 / INT64 | 8 B | 8 | 64 | 512 | |
| FP32 / INT32 / UINT32 | 4 B | 16 | 64 | 1024 | |
| FP16 / BF16 / INT16 | 2 B | 32 | 64 | 2048 | |
| FP8 (E4M3/E5M2) / INT8 | 1 B | 64 | 64 | 4096 | |
| MXFP4 | 0.5 B | 128 | 64 | 8192 | 32 元素共享一个 8-bit scale |
| HiFP4 | 0.5 B | 128 | 64 | 8192 | 类似 MXFP4，指数编码不同 |

> 关键性质：**每行总是 512 bit**。所以从硬件读端口看，一颗 Tile 的
> "形状"就是固定的 64×512b，不同元素类型只是改变了 packing 方式。

### 4.2 架构 Tile vs 物理 Tile（重命名映射）

| 名称 | 数量 | 编号位宽 | 谁产生 |
|------|------|---------|--------|
| 架构 Tile（T-reg） | **32**（T0–T31） | 5 bit | 编译器 / ISA |
| 物理 Tile（PT-slot） | **256**（PT0–PT255） | 8 bit | Tile RAT 重命名 |

D2 阶段的 **Tile RAT** 把 5-bit 的 T-reg 编码翻译成 8-bit 物理 Tile 索引；
RS 中存的是物理 Tile 索引（8 bit）+ ready 位（1 bit），不存 4 KB 数据本身
——数据太大，**RS 不 capture 数据**，只在 issue 时用物理索引去
TRegFile-4K 端口取。

### 4.3 TRegFile-4K 的物理组织

`TRegFile-4K`（详见 `tregfile4k_v2.md`）的关键参数：

| 参数 | 值 |
|------|-----|
| SRAM 实例 | 256 × 512 bit（1R1W），共 64 个 |
| Bank | 64 个，按 8 group × 8 bank 组织 |
| 总容量 | 64 × 16 KB = **1 MB** |
| 读端口 | **R0–R7**，每个 512 B / cycle |
| 写端口 | **W0–W7**，每个 512 B / cycle |
| Calendar | **8 cycle 同步 epoch**：每 port 每 8 cy 接受 1 个 `reg_idx` |
| 单端口吞吐 | 1 Tile (4 KB) / 8 cycle |
| 总读带宽 | 8 × 512 B/cy = **4 KB / cy** |
| 总写带宽 | 8 × 512 B/cy = **4 KB / cy** |
| Bank 解码（v2） | `bank = 8·g + ((l + g) mod 8)`（对角线 skew） |

**Tile 在 64 banks 上的摆放**：把一颗 4 KB Tile 看作 8×8 个 64 B chunk
组成的网格，把 chunk 索引拆成 `g = chunk[5:3]`、`l = chunk[2:0]`，
通过对角 skew 映射到物理 bank。这个 skew 让 row-sweep 与 col-sweep
**同时无 bank conflict**，是 v2 在 v1 基础上的核心增量
（因此 v2 端口上多了一个 `is_transpose` 位，配合 `reg_idx` 一起 latch）。

### 4.4 谁在用这些端口？（Vector / Cube / MTE 共享）

`Davinci_supersclar.md` §9.2 给出了 8R + 8W 端口在三种情境下的分配：

| 端口 | Cube 活跃 (MXFP4) | Cube 活跃 (FP16/BF16/FP8) | Cube 空闲 |
|------|-------------------|---------------------------|-----------|
| **R0** | Cube A | Cube A | Vector / MTE 自由 |
| **R1–R2** | Cube B | Cube B | Vector / MTE 自由 |
| **R3–R4** | Cube B | 空闲 → Vector / MTE | Vector / MTE 自由 |
| **R5–R7** | Vector 源 / MTE 读 | Vector 源 / MTE 读 | 自由 |
| **W0** | Cube C drain | Cube C drain | 自由 |
| **W1–W7** | Vector 写 / MTE 写 | Vector 写 / MTE 写 | 自由 |

每个端口的工作模式都是 **epoch-locked**：一旦 latch 了 `reg_idx`，
该端口就被这颗 Tile 独占 8 cycle，期间 512 B/cy 流式输入或输出。
新的 `reg_idx` 写到端口的 `pending` 寄存器，下一个 epoch 边界提升为
`active`，实现"零 bubble back-to-back"。

### 4.5 Tile 的生命周期（无 ROB，纯引用计数）

```
  分配 (D2):    tile free-list 出队 → 作为某条指令的目的物理 Tile
                Tile RAT[Td] ← PT_new ; old PT 标 orphan
                refcount(源 PT) += 读者数
  写  (Exec):   Vector / Cube-drain / MTE-LD 把 4 KB 数据写到 PT
                完成时在 TCB 上广播 PT 的 8-bit tag
                Tile RAT ready[PT] ← 1
  读  (Issue):  RS 中带 PT 标记的指令检查 trdy 位 → ready 后去 RegFile 读
  释放:         PT 既被标记 orphan、又 refcount==0 → 归还 free-list
```

> **TILE.MOVE 的特例**：在 D2 直接把 `Tile RAT[Td]` 指向 `Tile RAT[Ts]`
> 当前的物理 Tile，并 `refcount += 1`。**不分配新 PT、不进 RS、不进
> 执行单元、不占 TRegFile 端口**——是字面意义上的"零周期指令"。
> 这是这颗核避免无谓 4 KB 拷贝的关键优化。

### 4.6 为什么 RS 里只存 tag 不存数据？

| RS | 单条 entry 大小 | 是否存数据 | 原因 |
|----|-----------------|------------|------|
| Scalar RS | ~170 bit | ✅ 存 64-bit data | 标量数据小，CDB snoop 直接捕获 |
| LSU RS | ~170 bit | ✅ 同上 | 同上 |
| Vector / Cube / MTE RS | ~80 bit | ❌ **不存** | 4 KB Tile 数据放不进 RS，issue 时再去 TRegFile 读 |

这就是为什么 tile-domain 的 wakeup 走的是 **TCB**（4 端口，仅广播
8-bit Tile tag），而不是带 64 bit 数据的 CDB。

### 4.7 一句话总结 TREG 的角色

> **TRegFile-4K = 整颗核的"片上数据中央仓库"**。Vector 单元、
> Cube MXU、MTE 引擎都通过 8R+8W 端口在这上面读写 4 KB Tile；
> Tile RAT (32→256) 提供乱序重命名；TCB 广播 8-bit Tile tag 完成
> 跨 RS 的 wakeup；引用计数负责回收物理 Tile。

---

## 5. 流水线骨架（与 PTO/ISA 关系图回顾）

```
  Front-end (顺序)                 Tile RAT 与 Scalar RAT 各管一边
  ┌──────────────────────────────────────────────────────────────────┐
  │ F1 → F2 → D1 → D2 → DS                                            │
  │  fetch  decode  rename  dispatch                                  │
  │           (4-wide)        ↓ 5 个 RS                                │
  └──────────────────────────────────────────────────────────────────┘
                  │                                                  │
                  │            乱序后端                              │
  ┌───────────────┴────────────────────────────────┬───────────────┐
  │ Scalar RS  LSU RS  Vector RS  Cube RS  MTE RS │  TRegFile-4K   │
  │   (32)     (24)     (16)       (4)     (16)   │  256 × 4KB     │
  └─────────────────┬───────────────┬─────────────┘  8R + 8W       │
                    │               │                              │
                    ▼               ▼                              │
              ┌──────────────────────────┐                        │
              │ 4×ALU/MUL/DIV/BRU  LSU   │      CDB 广播          │
              │ Vector  Cube(outerCube)  │      TCB 广播          │
              │ MTE                       │                        │
              └──────────────────────────┘                        │
```

12 级标量流水：`F1 → F2 → D1 → D2 → DS → IS → EX1–EX4 → WB`，
**没有 retire / ROB**——这颗核明确不要求精确异常，所以省掉了
重排序缓冲，乱序完成、引用计数回收物理寄存器，分支恢复完全靠
8 个 RAT checkpoint 一周期 flash-restore。

---

## 6. 配套阅读路径（建议顺序）

整理这套代码时建议按下面的顺序看，从顶到底逐层下沉：

1. **本文（`davinci/docs/ARCHITECTURE_OVERVIEW_zh.md`）** —— 全局观。
2. **`davinci/docs/MODULE_MAP.md`** —— RTL 模块清单与状态。
3. **`Davinci_supersclar.md` §1–§4** —— 顶层架构、ISA、流水线。
4. **`tregfile4k_v2.md` §1–§3** —— Tile 寄存器堆物理实现细节。
5. **`outerCube.md` §6** —— Cube 指令与 MXU 数据通路。
6. **`PTOISA/PTOISA.md` + `PTOISA/conventions_zh.md`** —— 软件视角 ISA。
7. **`davinci/common/parameters.py`** —— 全局参数表。
8. **`davinci/davinci_top.py`** —— 顶层接线全貌（每段都有 `═══` 分节）。
9. 各子模块逐个读：`frontend/ → dispatch/ → backend/*_rs/ → backend/*_unit/`。

---

## 7. 一页"快查"备忘

```
  指令格式   : 32-bit 定长，opcode[6:5] 区分 domain
               00/01 标量, 10 向量/MTE, 11 Cube
  架构寄存器 : X0–X31 (5b) + T0–T31 (5b)
  物理寄存器 : P0–P127 (7b) + PT0–PT255 (8b)
  Fetch 宽度 : 4 instr / cycle (16 B fetch block)
  发射宽度   : 4 ALU + 1 MUL/DIV + 1 BRU + 2 LSU + 1 VEC + 1 CUBE + 2 MTE
  完成总线   : CDB (6 端口, 7-bit tag + 64-bit data)
               TCB (4 端口, 8-bit tile tag, no data)
  Tile 容量  : 4 KB / Tile, 64 行 × 512 bit / 行
  TRegFile   : 256 × 4 KB = 1 MB, 8R + 8W, 8-cycle epoch
  Branch 恢复: 1-cycle dual-RAT flash-restore, 8 个 checkpoint
  No ROB     : 引用计数回收物理寄存器，无 retire / 无精确异常
```
