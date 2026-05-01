# Davinci-v2 VTG Vector Micro-Instructions — SIMD-Group Execution Model

> **Document ID**: DSP-002
> **Version**: v1.0
> **Date**: 2026-05-02
> **Status**: Proposal
> **Target**: `pyCircuit/designs/outerCube/Davinci_superscalar_v2.md`
> **Change Point**: #2 — Add VTG (Vector Thread Group) vector micro-instructions with SIMD-group execution and pre-allocated micro-instruction buffer in the vector ALU; enable warp-grouped execution across VTGs inside a tile
> **Related**: [`Davinci_superscalar_v2.md`](Davinci_superscalar_v2.md) · [`Davinci_vector_micro_instructions_v1.md`](../Davinci_vector_micro_instructions_v1.md) · [`vector4k_v2.md`](vector4k_v2.md) · [`tregfile4k_v2.md`](tregfile4k_v2.md) · [`PTOAS/docs/vpto-spec.md`](PTOAS/docs/vpto-spec.md)

---

## 1. Motivation

### 1.1 Current Davinci-v2 Vector Execution Model

Davinci-v2 currently executes vector instructions as **full-tile operations** on VEC-4K-v2:

| Dimension | Current Davinci-v2 | Target |
|-----------|--------------------|--------|
| Tile operand size | 4 KB (full tile) | 4 KB tile = 16×256 B or 8×512 B VTG |
| Execution unit | One full-tile VEC op per cycle | One or more SIMD groups per VTG per cycle |
| Scheduling unit | One tile per VEC issue | Multiple VTGs inside one tile, round-robin |
| Loop handling | Scalar outer loop + VEC op per strip | Loop counters in GVIQ prefix; hardware rotates across VTGs |
| Micro-instruction storage | Decoded opcode in Vector RS entry | **Pre-allocated micro-instruction buffer in vector ALU** |
| Vector ISA surface | Full-tile `T*` ops only | VTG-relative `V*` micro-instructions + full-tile `T*` ops |
| Issue width | 1 VEC-4K-v2 tile op / cycle | 1 VTG micro-op / VEC beat; paired G256 = 2 VTGs / beat |

The current model treats every VEC instruction as a whole-tile strip-walk. For large matrices, this is efficient. For AI kernels with strip-mined inner loops where the same operation is applied across multiple 256 B or 512 B slices of a tile, the current model requires the compiler to manually generate repeated tile ops with different effective addresses.

### 1.2 What This Change Adds

This change introduces **VTG (Vector Thread Group) vector micro-instructions** — a warp-grouped execution model where:

- One 4 KB tile is partitioned into 16×256 B or 8×512 B **Vector Thread Groups (VTGs)**
- Each VTG carries loop/thread counter state in the GVIQ entry prefix
- A **micro-instruction buffer** is pre-allocated in the vector ALU, shared by all VTGs in the same tile group
- The vector ALU scheduler rotates through ready VTGs, issuing the same micro-instruction list across each VTG in turn — similar to warp-scheduling in GPU-style SIMD
- The existing VEC-4K-v2 ALU datapath (SA/SB/SC staging, 128-lane SIMD groups, 512 B/cycle throughput) is reused with minimal changes

This closes the gap between the existing full-tile VEC model and the warp-grouped model described in `Davinci_vector_micro_instructions_v1.md`.

---

## 2. Concepts

### 2.1 SIMD Group

A **SIMD group** is the fundamental execution granularity of the VEC-4K-v2 datapath.

The VEC-4K-v2 ALU operates on **128 lane groups** per 512 B VTG beat:

| Element type | Element size | Lanes per 512 B VTG | Lanes per 256 B VTG |
|-------------|--------------|---------------------|---------------------|
| FP32 / INT32 | 4 B | **128** | 64 |
| FP16 / BF16 | 2 B | **256** | 128 |
| FP8 | 1 B | **512** | 256 |
| FP4 | 0.5 B | **1024** | 512 |

Each lane computes an independent element. All 128 lanes execute the same micro-instruction in one VEC beat. The SIMD group is the vector ALU's native execution width — it is not a software-visible register, but the hardware's internal lane organization.

The **SIMD group concept** in this document refers to:
1. The 128-lane execution unit inside VEC-4K-v2
2. The architectural convention that a VTG (256 B or 512 B) maps to one or two SIMD group beats

### 2.2 Vector Thread Group (VTG)

A **Vector Thread Group (VTG)** is a warp-like scheduling context for vector micro-instructions. It is:

- A sub-portion of a 4 KB tile register: **256 B** (in `G256` mode) or **512 B** (in `G512` mode)
- A scheduling unit: the GVIQ entry prefix carries `group_id`, `thread_id`, `iter0..iter3` loop counters, and `active_lanes` — these state fields belong to a specific VTG
- A rename unit: VTG operands are tile-relative (`T4.g2` means tile T4, group g2), resolved through the Tile RAT at rename time
- A write domain: VTG writes only affect the selected 256 B or 512 B sub-range; other VTGs in the same tile are preserved

VTGs share a **micro-instruction buffer** (see §6) — the same micro-op list is pre-allocated once per tile group and referenced by all VTGs via their GVIQ entry's `block_id` pointer.

### 2.3 Relationship Between SIMD Group and VTG

| Dimension | SIMD group | VTG |
|-----------|-----------|-----|
| Size | Hardware lane count (128 lanes × 4 B = 512 B) | Software scheduling unit (256 B or 512 B) |
| Scope | VEC-4K-v2 ALU internal | IQ, rename, TRegFile, scheduler |
| Content | One beat of elementwise SIMD computation | One warp-like execution context (data + loop counters + predicates) |
| Mapping | In `G512` mode: 1 VTG = 1 SIMD group beat | In `G256` mode: 1 VTG = 1/2 SIMD group beat (two VTGs share one beat) |
| State | No software state | `group_id`, `thread_id`, `iter0..iter3`, `active_lanes`, `pc_index` |

### 2.4 Micro-Instruction Buffer

The **micro-instruction buffer** is a pre-allocated buffer in the vector ALU datapath. It is **shared** by all VTGs executing the same micro block and stores the decoded micro-instruction list.

Key properties:
- **Pre-allocated**: When a vector micro block enters the GVIQ, the compiler/front-end allocates a micro-instruction buffer entry and writes the decoded micro-op list once
- **Shared by VTGs**: All VTGs in the same tile group share the buffer via `block_id` pointer in their GVIQ entry prefix
- **Located in VEC ALU**: The buffer sits at the vector ALU input stage, accessible to the GVIQ issue logic and the VEC microcode controller
- **Format**: Each micro-instruction entry contains `{opcode, elem_type, pred_mode, src_vtg_refs, dst_vtg_ref, scalar_src, imm}` — all the fields needed to drive the VEC-4K-v2 staging and ALU without re-decoding

---

## 3. Architectural Model

### 3.1 VTG Storage Mapping

A 4 KB tile register is partitioned as follows:

**`G256` mode (16 VTGs per tile):**

```
byte[0..255]      → g0   (VTG 0, 256 B)
byte[256..511]    → g1   (VTG 1, 256 B)
byte[512..767]    → g2   (VTG 2, 256 B)
...
byte[3840..4095]  → g15  (VTG 15, 256 B)
```

**`G512` mode (8 VTGs per tile):**

```
byte[0..511]      → g0   (VTG 0, 512 B)
byte[512..1023]   → g1   (VTG 1, 512 B)
byte[1024..1535]  → g2   (VTG 2, 512 B)
...
byte[3584..4095]  → g7   (VTG 7, 512 B)
```

The group mode is a metadata field on the tile (or per-micro-block), set by the compiler and consulted at rename and issue time.

### 3.2 VTG Metadata

Each VTG carries the following metadata (stored in the **VTG Metadata Table**):

```
VTGMeta {
  valid:      1 b,    // VTG contains defined data
  kind:       3 b,    // VEC | PRED | WIDE_LO | WIDE_HI | ALIGN_LD | SCRATCH | UNDEF
  group_mode: 1 b,    // G256 = 0, G512 = 1
  elem_type:  4 b,    // FP32/FP16/FP8/FP4/INT32/...
  active_bytes: 10 b, // 0..256 (G256) or 0..512 (G512)
  pred_granule: 2 b,  // predicate grouping: 8/16/32-bit lane granularity
  pred_mode:   1 b,  // 0 = zeroing, 1 = merging (default)
  defined:     1 b,
  dirty:      1 b,
}
```

The VTG Metadata Table has 16 entries per physical tile (one per VTG in `G256` mode). In `G512` mode, entries g0/g2/g4/g6 are used and entries g1/g3/g5/g7 are sub-entries of pairs.

### 3.3 Top-Level Block Diagram

```
Scalar/Tile Front-End + D1/D2/D3 Rename
           |
           | VTG vector micro-instructions (VADD, VMUL, VLD, ...)
           v
+--------------------------------------------------+
|  Vector Micro Block Builder                        |
|  - Groups consecutive VTG micro-instructions       |
|  - Assigns block_id (micro block identifier)      |
|  - Creates loop/thread counter frames             |
+------------------------+-------------------------+
                         |
                         | block_id + decoded micro-op list
                         v
+--------------------------------------------------+
|  Micro-Instruction Buffer (in vector ALU)          |
|  - Pre-allocated, shared by all VTGs in block     |
|  - Contains decoded {opcode, elem_type, pred_mode,|
|    src/dst VTG refs, scalar_src, imm} per entry   |
|  - Referenced by GVIQ entries via block_id +      |
|    pc_index (micro-instruction pointer)            |
+------------------------+-------------------------+
                         |
                         | GVIQ entries with {block_id, pc_index,
                         | group_id, thread_id, iter0..iter3,
                         | active_lanes, VTG operands}
                         v
+--------------------------------------------------+
|  Grouped Vector Issue Queue (GVIQ)                |
|  - 32 entries, 1-wide VTG issue per cycle         |
|  - Entry prefix: block_id, pc_index, group_id,    |
|    thread_id, loop/thread counters                 |
|  - Wakeup: TRegFile tile tag → Ready Table-style  |
|    VTG ready bits per tile                        |
+------------------------+-------------------------+
                         |
                         | block_id → micro-instruction buffer lookup
                         | VTG operands → TRegFile Group Read Adapter
                         v
+--------------------------------------------------+
|  TRegFile Group Read Adapter                       |
|  - Fetches source tile (4 KB)                     |
|  - Selects 512 B VTG (G512) or 256 B VTG (G256)  |
|  - Delivers to SA/SB/SC staging registers         |
+------------------------+-------------------------+
                         |
                         | SA/SB/SC staging → VEC-4K-v2 ALU
                         | block_id + pc_index → micro-instruction buffer
                         v
+--------------------------------------------------+
|  VEC-4K-v2 ALU                                    |
|  - 128-lane SIMD groups per beat                   |
|  - Executes VTG micro-instruction from buffer      |
|  - Predicate merge/zeroing handled per lane         |
|  - Loop counter broadcast via SX/SY staging       |
+------------------------+-------------------------+
                         |
                         | result VTG → TRegFile Group Write Adapter
                         v
+--------------------------------------------------+
|  TRegFile Group Write Adapter                      |
|  - Writes 256 B or 512 B result into destination VTG|
|  - Preserves other VTGs in the tile               |
|  - Updates VTG metadata                           |
+------------------------+-------------------------+
                         |
                         | pc_index++, advance loop counters
                         | rotate to next ready VTG
                         v
                    [GVIQ or retire]
```

---

## 4. Micro-Instruction Buffer

### 4.1 Buffer Organization

The micro-instruction buffer is a **compile-time / decode-time allocated** structure in the vector ALU. It is not a circular buffer or a typical instruction RAM — it is a **set-associative buffer** keyed by `(block_id, VTG_id)`.

```
MicroInstructionBuffer {
  depth:    16 entries (max concurrent micro blocks)
  assoc:    2-way set associative
  block_id: u12  [key field]

  entry[N] {
    valid:       1 b
    block_id:    u12  [tag]
    pc_limit:    u8   [micro-instruction count - 1]
    micro_ops:   array[64] of MicroOpEntry  // max 64 micro-ops per block
  }
}

MicroOpEntry {
  opcode:     u12,    // VADD / VMUL / VCMP / VLD / VST / ...
  elem_type:  u4,     // FP32 / FP16 / FP8 / FP4 / INT32 / ...
  pred_mode:  u1,     // 0 = zeroing, 1 = merging
  src0_ref:   VTGRef, // tile relative (Tg, gN) or scalar
  src1_ref:   VTGRef | Scalar | Imm | None
  src2_ref:   VTGRef | Scalar | Imm | None
  dst_ref:    VTGRef
  pred_ref:   VTGRef | implicit_all_true
  addr_mode:  u2,     // strided / indexed / scalar_base / ...
  imm:        i32,    // immediate offset
  fault_policy: u1,   // checked / unchecked
}
```

### 4.2 Buffer Allocation

Buffer allocation happens at decode / front-end grouping time:

1. The **Vector Micro Block Builder** identifies a contiguous sequence of VTG micro-instructions that share the same tile group and loop structure
2. It assigns a `block_id` (12-bit, up to 4096 concurrent micro blocks in flight)
3. It decodes the micro-instruction list and writes each `MicroOpEntry` into the buffer at `buffer[block_id % depth][way].micro_ops[pc_index]`
4. Each GVIQ entry for a VTG in this block carries `{block_id, pc_index=0}` as its micro-instruction pointer

### 4.3 Buffer Access at Issue

At P1/I1 issue time, the GVIQ winner's `{block_id, pc_index}` fields drive a **single-cycle buffer lookup**:

```
micro_op = micro_inst_buffer.lookup(block_id, pc_index)
```

The `micro_op` content drives:
- VEC staging control (SA/SB/SC muxing, transpose flags)
- ALU opcode and element-type configuration
- Predicate mode and fault policy
- Destination VTG writeback routing

No re-decode is needed at issue time — the buffer entry is pre-decoded.

### 4.4 Buffer Sharing Across VTGs

All VTGs in the same tile group share the **same micro-instruction buffer entry** for a given `block_id`. Each VTG has its own `pc_index` in the GVIQ entry prefix, so VTGs can be at different points in the micro-instruction stream (e.g., one VTG is on iteration 3 while another is on iteration 7).

When the GVIQ scheduler rotates to a new VTG:
1. It reads `{block_id, pc_index, group_id, thread_id, iter0..iter3}` from the winning GVIQ entry
2. It performs `micro_inst_buffer.lookup(block_id, pc_index)` to get the `MicroOpEntry`
3. It drives the VEC staging and ALU with the micro-op content + VTG-specific operands

---

## 5. GVIQ — Grouped Vector Issue Queue

### 5.1 GVIQ Entry Format

```
GVIQEntry {
  // ── Micro-instruction pointer ──────────────────────
  valid:       1 b
  block_id:    u12,   // index into micro-instruction buffer
  pc_index:    u8,    // current micro-instruction within block (0..63)

  // ── VTG identity ───────────────────────────────────
  tile_group:  u5,    // architectural tile T0..T31
  phys_tile:   u8,    // physical tile PT0..PT255 (after Tile RAT rename)
  group_id:    u4,    // VTG index: 0..15 (G256) or 0..7 (G512)
  group_mode:  u1,    // 0 = G256, 1 = G512

  // ── Thread / loop context ───────────────────────────
  thread_id:   u8,    // scheduler context (usually = group_id)
  iter0:       u16,   // loop counter 0
  iter1:       u16,   // loop counter 1
  iter2:       u16,   // loop counter 2
  iter3:       u16,   // loop counter 3
  active_lanes: u16,  // active lane count or mask
  active_group_mask: u16, // which VTG groups are active in this block

  // ── VTG operand ptags (after rename) ──────────────
  src0_ptag:   u8,    // physical tile tag for src0 VTG
  src1_ptag:   u8,    // physical tile tag for src1 VTG
  src2_ptag:   u8,    // physical tile tag for src2 VTG
  pred_ptag:   u8,    // physical tile tag for predicate VTG
  dst_ptag:    u8,    // physical tile tag for destination VTG
  has_dst:     1 b,
  src_ready:   (4,),  // VTG-ready bits: src0/1/2/pred ready

  // ── Scheduling ─────────────────────────────────────
  branch_tag:  u3,    // branch tag for speculation gating
  vtg_ready:   1 b,   // all source VTGs ready + loop counters ready
}
```

### 5.2 VTG Wakeup

Unlike scalar ptag wakeup via the Ready Table, VTG wakeup is simpler because **tile registers are the operand unit**:

```
vtg_ready = src_ready[0] & src_ready[1] & src_ready[2] & src_ready[3]
           & loop_counters_ready
```

`src_ready[i]` is set when `src_i_ptag` receives a writeback from a previous VTG operation. The VTG Ready Table is a **256-entry bitmap** (one bit per physical tile PT0..PT255), similar in structure to the scalar Ready Table.

### 5.3 Issue Rules

| Rule | Description |
|------|-------------|
| IQ-1 | `pc_index` must be within the buffer entry's `pc_limit` for the given `block_id` |
| IQ-2 | All source VTG `src_ready` bits must be set |
| IQ-3 | Loop counter `iter*` must be non-zero (or the instruction does not consume a loop counter) |
| IQ-4 | The GVIQ is 1-wide: at most one VTG micro-op issues per cycle |
| IQ-5 | The VEC-4K-v2 ALU is single-ported per VTG: one VTG micro-op per VEC beat |
| IQ-6 | Paired `G256` issue (optional v1): two independent 256 B VTGs may share one 512 B SIMD group beat if their `opcode`, `elem_type`, and `pred_mode` all match |

---

## 6. Vector Micro-Instruction Families

### 6.1 Instruction Syntax

All VTG vector micro-instructions use the following syntax:

```
VINST.type  Td.gN, Ts0.gM, Ts1.gP, Tp.gQ
```

Where:
- `Td.gN` = destination VTG (tile `Td`, group `gN`)
- `Ts0.gM`, `Ts1.gP` = source VTGs
- `Tp.gQ` = predicate VTG
- `.type` = element type: `.F32`, `.F16`, `.BF16`, `.F8`, `.F4`, `.I32`, `.I16`, `.I8`

### 6.2 ALU Instructions

#### 6.2.1 Elementwise ALU

| Instruction | Syntax | Operation |
|-------------|--------|-----------|
| `VADD` | `VADD.type Td, Ts0, Ts1, Tp` | `Td[i] = Tp[i] ? (Ts0[i] + Ts1[i]) : merge(Td[i])` |
| `VSUB` | `VSUB.type Td, Ts0, Ts1, Tp` | `Td[i] = Tp[i] ? (Ts0[i] - Ts1[i]) : merge(Td[i])` |
| `VMUL` | `VMUL.type Td, Ts0, Ts1, Tp` | `Td[i] = Tp[i] ? (Ts0[i] * Ts1[i]) : merge(Td[i])` |
| `VDIV` | `VDIV.type Td, Ts0, Ts1, Tp` | `Td[i] = Tp[i] ? (Ts0[i] / Ts1[i]) : merge(Td[i])` |
| `VMIN` | `VMIN.type Td, Ts0, Ts1, Tp` | `Td[i] = Tp[i] ? min(Ts0[i], Ts1[i]) : merge(Td[i])` |
| `VMAX` | `VMAX.type Td, Ts0, Ts1, Tp` | `Td[i] = Tp[i] ? max(Ts0[i], Ts1[i]) : merge(Td[i])` |
| `VABS` | `VABS.type Td, Ts0, Tp` | `Td[i] = Tp[i] ? abs(Ts0[i]) : merge(Td[i])` |
| `VNEG` | `VNEG.type Td, Ts0, Tp` | `Td[i] = Tp[i] ? -Ts0[i] : merge(Td[i])` |

#### 6.2.2 Scalar-Broadcast ALU

| Instruction | Syntax | Operation |
|-------------|--------|-----------|
| `VADDS` | `VADDS.type Td, Ts, Xs, Tp` | `Td[i] = Tp[i] ? (Ts[i] + Xs) : merge(Td[i])` |
| `VMULS` | `VMULS.type Td, Ts, Xs, Tp` | `Td[i] = Tp[i] ? (Ts[i] * Xs) : merge(Td[i])` |
| `VMAXS` | `VMAXS.type Td, Ts, Xs, Tp` | `Td[i] = Tp[i] ? max(Ts[i], Xs) : merge(Td[i])` |

Scalar operands (`Xs`) come from the scalar register file (atag → ptag) and are broadcast to all 128 SIMD lanes via the SX/SY staging registers.

#### 6.2.3 Compare and Select

| Instruction | Syntax | Operation |
|-------------|--------|-----------|
| `VCMP.{LT/LE/GT/GE/EQ/NE}` | `VCMP.cmp.type Tpd, Ts0, Ts1, Tp` | Predicate VTG `Tpd` receives comparison result; `Tpd[i] = cmp(Ts0[i], Ts1[i])` |
| `VSEL` | `VSEL.type Td, Ts0, Ts1, Tp` | `Td[i] = Tp[i] ? Ts0[i] : Ts1[i]` (predicate selects between two source VTGs) |
| `VMERGE` | `VMERGE.type Td, Ts, Tp` | Merging-mode fill: `Td[i] = Tp[i] ? Ts[i] : Td[i]` (reads old destination) |

#### 6.2.4 Conversion

| Instruction | Syntax | Operation |
|-------------|--------|-----------|
| `VCVT` | `VCVT.dtype.stype Td, Ts, Tp` | `Td[i] = cast<Tdtype>(Ts[i])` with saturation and rounding |
| `VROUND` | `VROUND.type Td, Ts, Tp` | `Td[i] = round(Ts[i])` with configurable rounding mode |
| `VTRUNC` | `VTRUNC.type Td, Ts, Tp` | `Td[i] = truncate(Ts[i])` |

#### 6.2.5 Math

| Instruction | Syntax | Operation |
|-------------|--------|-----------|
| `VSQRT` | `VSQRT.type Td, Ts, Tp` | `Td[i] = Tp[i] ? sqrt(Ts[i]) : merge(Td[i])` |
| `VEXP` | `VEXP.type Td, Ts, Tp` | `Td[i] = Tp[i] ? exp(Ts[i]) : merge(Td[i])` |
| `VLOG` | `VLOG.type Td, Ts, Tp` | `Td[i] = Tp[i] ? log(Ts[i]) : merge(Td[i])` |
| `VRELU` | `VRELU.type Td, Ts, Tp` | `Td[i] = Tp[i] ? (Ts[i] > 0 ? Ts[i] : 0) : merge(Td[i])` |

### 6.3 Predicate Instructions

| Instruction | Syntax | Operation |
|-------------|--------|-----------|
| `PLT` | `PLT Tpd, iter0, Tp` | `Tpd[i] = (i < iter0) ? 1 : 0` — loop counter predicate |
| `PAND` | `PAND Tpd, Tp0, Tp1` | `Tpd[i] = Tp0[i] & Tp1[i]` |
| `POR` | `POR Tpd, Tp0, Tp1` | `Tpd[i] = Tp0[i] \| Tp1[i]` |
| `PXOR` | `PXOR Tpd, Tp0, Tp1` | `Tpd[i] = Tp0[i] ^ Tp1[i]` |
| `PNOT` | `PNOT Tpd, Tp` | `Tpd[i] = ~Tp[i]` |

Predicate VTG kind: predicates are stored as VTGs of kind `PRED`. A predicate VTG for 128 lanes requires 128 bits (16 bytes), stored in the low 16 bytes of a 256 B VTG slot.

### 6.4 Memory Instructions

| Instruction | Syntax | Operation |
|-------------|--------|-----------|
| `VLD` | `VLD.type Td.gN, [Xbase + Xoff], Tp` | Load 256/512 B under predicate into `Td.gN` |
| `VST` | `VST.type Ts.gN, [Xbase + Xoff], Tp` | Store 256/512 B from `Ts.gN` under predicate |
| `VLDSTRIDE` | `VLDSTRIDE.type Td, Xbase, Xstride, Xcount, Tp` | Strided load: `Td[i] = mem[Xbase + i*Xstride]` |
| `VSTSTRIDE` | `VSTSTRIDE.type Ts, Xbase, Xstride, Xcount, Tp` | Strided store |
| `PGATHER` | `PGATHER.type Tpd, [Xbase + Ts*esize], Tp` | Gather predicate: `Tpd[i] = mem[Xbase + Ts[i]*esize]` |

**Inactive-lane fault suppression**: Vector loads/stores MUST NOT fault for inactive lanes (predicate bit = 0). The LSU checks the active-lane mask before performing each lane's address calculation.

### 6.5 Wide / Reduction Instructions

| Instruction | Syntax | Operation |
|-------------|--------|-----------|
| `VREDUCE_ADD` | `VREDUCE_ADD.type Xd, Ts, Tp` | `Xd = Σ(Ts[i] * Tp[i])` — scalar reduction output |
| `VREDUCE_MAX` | `VREDUCE_MAX.type Xd, Ts, Tp` | `Xd = max(Ts[i] * Tp[i])` |
| `WADD` | `WADD.type Td, Ts0, Ts1, Tp` | Wide add: result spans 2 VTGs for extended precision |

### 6.6 Micro-Instruction Count

| Category | Count |
|---------|-------|
| Elementwise ALU | 11 |
| Scalar-broadcast ALU | 3 |
| Compare and Select | 4 |
| Conversion | 3 |
| Math | 4 |
| Predicate | 5 |
| Memory | 5 |
| Reduction / Wide | 3 |
| **Total** | **38** |

---

## 7. Execution Pipeline

### 7.1 VTG Micro-Op Lifecycle

```
Cycle N:   FETCH     — F0→F1→F2→F3→IB→F4: instruction fetch
Cycle N+6: D1        — decode + atag/ptag rename (scalar)
Cycle N+9: D2/D3     — Tile RAT rename for VTGs; block_id allocated
Cycle N+11:S1/S2     — GVIQ entry write; micro-instruction buffer populated
Cycle N+12:P1        — GVIQ pick: select oldest-ready VTG micro-op
Cycle N+13:I1        — TRegFile read for source tiles; buffer lookup (block_id, pc_index)
Cycle N+14:I2        — Issue confirm; SA/SB/SC staging populated
Cycle N+15:E1        — VEC-4K-v2 ALU begins execution (128-lane SIMD group)
Cycle N+22:W1        — Writeback; VTG ready bits updated; pc_index++, loop counters updated
```

### 7.2 VTG Rotation Scheduling

When multiple VTGs in the same tile group are active, the GVIQ scheduler rotates across them:

```
while any VTG active:
    # Pick the oldest-ready VTG (age = (entry.rid - head_rid) mod 64)
    winner = gviq.pick_oldest_ready()

    # Read micro-instruction from buffer
    micro_op = buffer.lookup(winner.block_id, winner.pc_index)

    # Read VTG operands from TRegFile
    SA = TRegFile.read(winner.src0_ptag)   # Full 4 KB tile
    SB = TRegFile.read(winner.src1_ptag)

    # Select VTG sub-range (256 B or 512 B)
    SA_vtg = select_vtg(SA, winner.group_id, winner.group_mode)
    SB_vtg = select_vtg(SB, winner.group_id, winner.group_mode)

    # Execute
    result = vec_alu.execute(micro_op.opcode, SA_vtg, SB_vtg, micro_op.pred_mode)

    # Write back to destination VTG
    TRegFile.write_vtg(winner.dst_ptag, winner.group_id, result)

    # Advance
    winner.pc_index++
    if loop_end(winner):
        winner.iterN--
        winner.pc_index = loop_start
    if all_iters_done(winner):
        winner.valid = 0   # Retire GVIQ entry
```

### 7.3 TRegFile Group Read/Write Adapters

**Group Read Adapter** (TRegFile → VEC staging):
```
input:  full_tile_data[4096 B], group_id, group_mode
G256:   vtg_data[256 B] = full_tile_data[group_id * 256 : (group_id+1) * 256]
G512:   vtg_data[512 B] = full_tile_data[group_id * 512 : (group_id+1) * 512]
output: vtg_data → SA or SB staging register
```

**Group Write Adapter** (VEC result → TRegFile):
```
input:  vtg_result[256/512 B], dst_ptag, group_id, group_mode, other_vtgs_preserve
G256:   full_tile = merge(vtg_result, existing_tile, group_id)  # preserve other 15 VTGs
G512:   full_tile = merge(vtg_result, existing_tile, group_id)  # preserve other 7 VTGs
TRegFile.write(dst_ptag, full_tile)
update VTG_metadata[dst_ptag][group_id] = {valid=1, defined=1, dirty=1}
```

---

## 8. Interaction with Existing VEC-4K-v2

### 8.1 Staging Register Reuse

The existing VEC-4K-v2 staging registers are reused:

| Staging Register | VTG Micro-Op Use |
|-----------------|-----------------|
| `SA` | Source VTG 0 (or old destination for merging) |
| `SB` | Source VTG 1 (or scalar broadcast expansion) |
| `SC` | Predicate VTG (or third source VTG for wide ops) |
| `SX / SY` | Scalar operand / loop counter broadcast |
| `SOP` | Micro-instruction opcode and control from buffer |

### 8.2 ALU Lane Mapping

| VTG Mode | VTG Size | SIMD Group Beats | ALU Throughput |
|----------|----------|-----------------|---------------|
| `G256` | 256 B | 1/2 beat (two VTGs share one beat) | 2 VTGs / VEC beat (if paired) |
| `G512` | 512 B | 1 full beat | 1 VTG / VEC beat |

For `G256` with paired issue: the scheduler selects two VTGs with matching `{opcode, elem_type, pred_mode}` and issues them together, filling the full 512 B SIMD group beat.

### 8.3 VEC Microcode Interaction

The VEC microcode controller drives the ALU based on the `MicroOpEntry` from the buffer. The microcode program is keyed by `(opcode, elem_type, pred_mode)` and configures:
- SA/SB/SC muxing and transpose flags
- ALU opcode and lane configuration
- Predicate merge/zeroing mode per lane
- Destination writeback routing

---

## 9. Integration with Davinci_superscalar_v2.md

The following sections of `Davinci_superscalar_v2.md` require updates:

| Section | Update |
|---------|--------|
| §1 Key Parameters | Add VTG/GVIQ parameters: GVIQ depth, micro-buffer depth, VTG count per tile, G256/G512 mode |
| §2.2 Vector ISA | Add VTG vector micro-instruction families (V* prefix) alongside existing full-tile T* ops |
| §3 Block Diagram | Add Vector Micro Block Builder, Micro-Instruction Buffer, GVIQ, VTG Metadata Table, Group Read/Write Adapters |
| §4 Pipeline | Add VTG micro-op lifecycle stages |
| §6 Decode & Rename | Add VTG operand decode, Tile RAT interaction, block_id allocation |
| §7 Dispatch & Issue | Add GVIQ entry format, VTG wakeup, rotation scheduling, micro-buffer lookup |
| §8.3 Vector Unit | Add VTG execution mode, G256/G512 SIMD group mapping, micro-instruction buffer integration |
| §9 Register Files | Define VTG as sub-unit of tile register; add VTG Metadata Table |
| §10 OoO Model | Add VTG dependency tracking, VTG-ready bits |
| §12 Memory | Add VTG load/store, inactive-lane fault suppression |

---

## 10. Key Parameters

| Parameter | Value |
|-----------|-------|
| VTG modes | `G256` (16 VTGs/tile) · `G512` (8 VTGs/tile) |
| GVIQ depth | 32 entries |
| GVIQ issue width | 1 VTG micro-op / cycle |
| Micro-instruction buffer depth | 16 entries |
| Micro-instructions per block | max 64 |
| VTG operand size | 256 B (`G256`) or 512 B (`G512`) |
| SIMD lanes per VTG beat | 128 (FP32), 256 (FP16/BF16), 512 (FP8), 1024 (FP4) |
| VTG Metadata Table | 16 entries / physical tile |
| VTG ready bitmap | 256 bits (one per PT0..PT255) |
| Predicate VTG size | 16 B (128 bits) stored in low 16 B of 256 B VTG slot |
| Loop counters per GVIQ entry | 4 × 16-bit |

---

## 11. Comparison: Full-Tile vs. VTG Micro-Op Execution

| Dimension | Full-tile T* (current) | VTG V* micro-op (this change) |
|-----------|------------------------|-------------------------------|
| Operand size | 4 KB (full tile) | 256 B or 512 B VTG |
| Scheduling unit | One tile per VEC op | One VTG per GVIQ entry |
| Loop handling | Scalar outer loop + repeated T* ops | Loop counters in GVIQ prefix; hardware rotation |
| Micro-instruction storage | Decoded opcode in Vector RS | Pre-allocated micro-instruction buffer in vector ALU |
| TRegFile access | Full 4 KB tile read/write | VTG sub-range read/write via adapters |
| ISA surface | `TADD`, `TMUL`, `TLOAD`, ... | `VADD`, `VMUL`, `VLD`, ... |
| Tile RAT rename | Full tile rename | Tile rename + VTG group_id index |
| Predicate handling | Per-element mask via SC staging | Per-VTG predicate VTG + SC staging |
| Throughput | 1 tile / 8 cycles (TRegFile epoch) | 1 VTG / VEC beat (paired G256 = 2 VTGs/beat) |
| Typical use case | Large matrix ops, GEMM | Strip-mined inner loops, elementwise on multiple slices |

---

## 12. Open Questions

| ID | Question | Priority |
|----|----------|----------|
| OQ-1 | Should `G256` paired issue (2 VTGs per beat) be v1 or v2? | High |
| OQ-2 | How does the GVIQ interact with the existing Vector RS (24 entries)? Are they unified or separate? | High |
| OQ-3 | Should the micro-instruction buffer be invalidated on a branch mispredict (like MapQ)? | Medium |
| OQ-4 | How many simultaneous micro blocks (`block_id` space) should be supported in flight? | Medium |
| OQ-5 | Does the VTG micro-op path share the VEC staging registers with the full-tile path, or are they separate? | Medium |
| OQ-6 | What is the exact fault reporting format for VTG micro-ops? (block_id, thread_id, group_id, lane) | Medium |
