══════════════════════════════════════════════════════════════════════════════
  VEC-4K-v2 datapath overview  (analyser model — see vector4k_v2.md)
══════════════════════════════════════════════════════════════════════════════
  TILE size              : 4096 B  = R · C · E   (E ∈ {1, 2, 4, 8} B for FP8 / FP16 / FP32 / FP64)
  Per register file      : 4096 B = 2048 × 2 B DFFs
  Tilelet (read port)    : 512 B / cycle, beats are sequential row-major
                           beat 0 : bytes    0..511
                           beat 1 : bytes  512..1023
                           ...
                           beat 7 : bytes 3584..4095
  Beats per tile         : 8

  Register files (4 × 4096 B = 16 KB total) :
    • RFS_A  : Operand-A staging RF      — input tile A
    • RFS_B  : Operand-B staging RF      — REPURPOSED:  acc[] + output tile D
    • RFA_A  : Accumulator RF A          — Acc ping-pong half 0 (legacy §5.5)
    • RFA_B  : Accumulator RF B          — Acc ping-pong half 1 (legacy §5.5)

  Sample tile shapes (each = 4 KB) :
    • (  64 × 64  , FP8 )  → row_bytes = 64 B
    • (  32 × 64  , FP16)  → row_bytes = 128 B
    • (   8 × 128 , FP32)  → row_bytes = 512 B
    • (   1 × 1024, FP32)  → row_bytes = 4096 B
    • ( 256 × 4   , FP32)  → row_bytes = 16 B
    • (  16 × 128 , FP16)  → row_bytes = 256 B

══════════════════════════════════════════════════════════════════════════════
  TREE DATAPATH  (Benes-like shrink-then-expand network)
══════════════════════════════════════════════════════════════════════════════

  INPUT side  (1024 B = 2 × 512 B from RFS_A and RFS_B / Acc):

      RFS_A 512 B tilelet              RFS_B 512 B tilelet  (= acc[] tilelet)
              │                                    │
              ▼                                    ▼
       ┌────────────────────────────────────────────────────┐
       │  L0 leaves :  N_in  bytes / elements               │
       │  N_in = 1024 / E   (E = bytes/elem)              │
       │  → FP8 : 1024 leaves    FP16 : 512 leaves          │
       │  → FP32:  256 leaves    FP64 : 128 leaves          │
       └────────────────────────────────────────────────────┘
              │
              ▼
       ┌────────────────────────────────────────────────────┐
       │  L1 .. L_log2(N_in) : reduction stages             │
       │    each stage = N/2 binary nodes                   │
       │    node ops : ADD | CMP_SWAP_MAX | CMP_SWAP_MIN |  │
       │               PASS_LEFT | PASS_RIGHT | MUX_BY_CTRL │
       │    per-level 'stop bit' : let sub-trees deliver    │
       │      K partial outputs at any chosen level.        │
       │    ⇒ for ROW_SUM_EXP  : K = rows_per_input_beat,   │
       │      sub-tree size = C input elements per row.     │
       └────────────────────────────────────────────────────┘
              │
              ▼
        K partial outputs at chosen stop level
        (K = rows in this beat ; each = 1 element of E_out precision)


  OUTPUT side (1024 B = 2 × 512 B back into RFS_B):

         ACC_SPILL[r] (1 element per row, E_out wide)
              │
              ▼
       ┌────────────────────────────────────────────────────┐
       │  L0 seeds :  K elements (one per output row)       │
       └────────────────────────────────────────────────────┘
              │
              ▼
       ┌────────────────────────────────────────────────────┐
       │  L1 .. L_log2(N_out) : broadcast stages            │
       │    each stage = 2× wider than previous             │
       │    node ops : FANOUT | ADD_THEN_FANOUT |           │
       │               CMP_SWAP_THEN_FANOUT | PERMUTE       │
       └────────────────────────────────────────────────────┘
              │
              ▼
       ┌────────────────────────────────────────────────────┐
       │  Lout leaves : N_out bytes / elements              │
       │  ⇒ ROW_SUM_EXP : C_out copies per row × K rows     │
       │    = 1024 B (or 512 B for one tilelet output)      │
       └────────────────────────────────────────────────────┘

  Combined REDUCE → BROADCAST  ≡  Benes-like network with arithmetic at the
  internal nodes. The same hardware also implements:

    OPERATION          | REDUCE side ops                  | BROADCAST side ops
    -------------------|----------------------------------|---------------------
    axis reduce sum    | ADD                              | (none — no broadcast)
    axis reduce max    | CMP_SWAP_MAX (keep larger)       | (none)
    axis reduce min    | CMP_SWAP_MIN (keep smaller)      | (none)
    column / row expand| (none — no reduce)               | FANOUT
    ROW_SUM_EXP        | ADD                              | FANOUT
    sort               | CMP_SWAP (full bitonic schedule) | PERMUTE
    merge_sort         | CMP_SWAP between two halves      | PERMUTE / FANOUT

══════════════════════════════════════════════════════════════════════════════
  BENES-LIKE TREE DATAPATH — detailed structure
  (drawn for N = 16 leaves; real datapath has N = 1024 / E_in leaves)
══════════════════════════════════════════════════════════════════════════════

  Per-format leaf count and stage depth:

      format   N_leaves  reduce stages  broadcast stages   total cy.
      ──────   ────────  ─────────────  ────────────────   ─────────
      FP8         1024     10               10                21*
      FP16         512      9                9                19*
      FP32         256      8                8                17*
      FP64         128      7                7                15*

      * = log2(N) reduce + 1 ACC_SPILL + log2(N) broadcast,
          but for ROW_SUM_EXP we always issue 8 reduce + 1 spill
          + 8 broadcast = 17 cycles regardless of N (compute is
          beat-pipelined; the tree depth is its critical path,
          not its issue rate).

  ─── REDUCE half  (binary fan-in, log2 N ADD/CMP_SWAP stages) ──────────

    Operand-A tilelet (512 B from RFS_A)
              +  Operand-B / Acc tilelet (512 B from RFS_B)
              =  N leaves total at L0

  L0    ● ● ● ● ● ● ● ● ● ● ● ● ● ● ● ●    leaves  (E_in-wide elements)
        └┬┘ └┬┘ └┬┘ └┬┘ └┬┘ └┬┘ └┬┘ └┬┘
  L1     ◉   ◉   ◉   ◉   ◉   ◉   ◉   ◉    N/2 = 8  ALU nodes
         └─┬─┘   └─┬─┘   └─┬─┘   └─┬─┘
  L2       ◉       ◉       ◉       ◉      N/4 = 4  ALU nodes
           └───┬───┘       └───┬───┘
  L3           ◉               ◉          N/8 = 2  ALU nodes
               └───────┬───────┘
  L4                   ◉                  N/16 = 1 ROOT node
                       │
                       ▼
                  1 scalar (cast E_in → E_out)

    Each ALU node :  2-input → 1-output  ("butterfly cell")
    opcode ∈ { ADD, CMP_SWAP_MAX, CMP_SWAP_MIN,
               PASS_LEFT, PASS_RIGHT, MUX_BY_CTRL }

    "Stop-level" mechanism (key for ROW_SUM_EXP with K rows / beat):
      - K=1 → take output at L4 (ROOT)   : 1 partial of N elems  reduced
      - K=2 → take outputs at L3         : 2 partials of N/2 each
      - K=4 → take outputs at L2         : 4 partials of N/4 each ★
              (e.g. (32,32,FP32) → 4 rows × 32 elems / beat)
      - K=8 → take outputs at L1         : 8 partials of N/8 each ★
              (e.g. (64,64,FP8)  → 8 rows × 64 elems / beat)

  ─── ACC_SPILL latch  (K-element wide register between halves) ─────────

                       ┌──────────────────────────────┐
                       │  K · E_out byte register     │
                       │  K = rows_per_input_beat     │
                       │  (≤ 1 KB for canonical cases)│
                       └──────────────────────────────┘
                       │  hides phase-1 → phase-2     │
                       │  RAW hazard on RFS_B         │
                       ▼

  ─── BROADCAST half  (binary fan-out, log2 N FANOUT stages) ────────────

  L4'                  ◎                  1 SEED  (acc[r] from spill)
               ┌───────┴───────┐
  L3'          ◎               ◎          2 FANOUT
           ┌───┴───┐       ┌───┴───┐
  L2'      ◎       ◎       ◎       ◎      4 FANOUT
         ┌─┴─┐   ┌─┴─┐   ┌─┴─┐   ┌─┴─┐
  L1'    ◎   ◎   ◎   ◎   ◎   ◎   ◎   ◎    8 FANOUT
        ┌┴┐ ┌┴┐ ┌┴┐ ┌┴┐ ┌┴┐ ┌┴┐ ┌┴┐ ┌┴┐
  L0'   ● ● ● ● ● ● ● ● ● ● ● ● ● ● ● ●   N=16 output leaves (E_out)
        ▼ ▼ ▼ ▼ ▼ ▼ ▼ ▼ ▼ ▼ ▼ ▼ ▼ ▼ ▼ ▼

    Output tilelet (1024 B back to RFS_B)

    Each FANOUT node :  1-input → 2-output  ("inverse butterfly cell")
    opcode ∈ { FANOUT,            (default — y0 = y1 = x)
               ADD_THEN_FANOUT,   (y0 = x+c0, y1 = x+c1)
               CMP_SWAP_THEN_FANOUT,
               PERMUTE          } (used by sort / merge_sort)

    Same "stop-level" lets BROADCAST start from any level :
      - K=1 seed at L4'  → fan out to all N leaves
      - K=4 seeds at L2' → each fans out to N/4 = 4 leaves ★
              (e.g. (32,32,FP32) → 4 rows × 32 copies / beat)

  ─── Node-internal architecture ────────────────────────────────────────

    REDUCE node  (2-in → 1-out)        BROADCAST node  (1-in → 2-out)

       a ──┐                                    ┌── y0
           ├─►─[ ALU ]─►─ y           x ──►─────┤
       b ──┘     ▲                              └── y1
                 │                              ▲
              opcode                            │
                                             opcode
    opcode ∈ {ADD, CMP_SWAP_*,         opcode ∈ {FANOUT,
              PASS_*, MUX_BY_CTRL}              ADD_THEN_FANOUT,
                                                CMP_SWAP_THEN_FANOUT,
                                                PERMUTE}

    Per-stage gate count ≈ N/2 cells × (adder + ctrl mux + opcode latch)
    Total tree gates    ≈ 2 · (N − 1) cells = O(N)

  ─── Recursive Benes(N) structure (for context / reference) ────────────

    A pure Benes(N) network is 2·log2(N) − 1 stages of 2×2 switches,
    arranged recursively :

      ┌──────────────────────────────────────────────────────────────┐
      │                                                              │
      │  in[0..N/2−1] ─┬─[2×2]─┬─►─ Benes(N/2) ─┬─[2×2]─┬─ out[..]   │
      │                │       │                │       │            │
      │                │       ▼                ▼       │            │
      │            cross    middle           middle   cross          │
      │           permute  sub-block       sub-block permute         │
      │                │       ▲                ▲       │            │
      │                │       │                │       │            │
      │  in[N/2..N−1] ─┴─[2×2]─┴─►─ Benes(N/2) ─┴─[2×2]─┴─ out[..]   │
      │                                                              │
      └──────────────────────────────────────────────────────────────┘

    Our REDUCE → SPILL → BROADCAST tree is the same back-to-back
    butterfly shape, but :
      • each 2×2 switch is replaced by an arithmetic node
        (ADD / CMP_SWAP / FANOUT instead of pure routing),
      • the central Benes(N/2) sub-block is collapsed into a single
        ACC_SPILL register (K · E_out bytes wide).

    The same hardware therefore covers reduce, broadcast, sort, and
    merge_sort by selecting per-stage opcodes from microcode, exactly
    as a permutation Benes covers any permutation by selecting per-
    switch settings from a routing table.

  ─── Wiring back to RFS_A / RFS_B (vector4k_v2.md §4.2) ────────────────

           RFS_A (4 KB)        RFS_B (4 KB)
              │                    │
              │ 512 B/cy           │ 512 B/cy  (acc[] read)
              ▼                    ▼
      ┌───────────────────────────────────┐
      │   REDUCE half  (1024 B input)     │
      └───────────────────────────────────┘
                       │
                       ▼  K partials
              ┌────────────────┐
              │   ACC_SPILL    │  K · E_out bytes
              └────────────────┘
                       │
                       ▼  K seeds
      ┌───────────────────────────────────┐
      │   BROADCAST half (1024 B output)  │
      └───────────────────────────────────┘
                       │
                       ▼ 512 B/cy  (RMW write back)
                  RFS_B (4 KB)

    REDUCE and BROADCAST halves share the same physical lanes ;
    a single beat uses EITHER reduce OR broadcast direction, not
    both (8 cy reduce, 1 cy spill, 8 cy broadcast = 17 cy total).

  ── NOTE on "Benes-like" terminology ──
    The shrink-then-expand SHAPE resembles Benes, but our tree does
    NOT use Benes' defining feature (per-stage 2×2 routing switches).
    See `--benes-analysis` for why this is essentially a fixed-wiring
    binary tree, NOT a Benes network — and why dropping the Benes
    framing removes ~10-20 % area and clarifies the design.

══════════════════════════════════════════════════════════════════════════════
  IS THE BENES NETWORK ACTUALLY USEFUL?  CAN WE ELIMINATE IT?
══════════════════════════════════════════════════════════════════════════════

  Short answer : YES, eliminate it.

  The "Benes-like" label has been a pedagogical conceit (the back-to-
  back butterfly SHAPE looks like Benes). In practice, our tree NEVER
  uses Benes' defining feature : arbitrary permutation routing via
  per-stage 2×2 switches. The connectivity is hardwired binary-tree
  fan-in / fan-out.

  ─── What pure Benes provides vs what we use ───────────────────────────

    feature                        Pure Benes(N)    Our tree
    ─────────────────────────────  ───────────────  ──────────────────────
    stages                         2·log2(N) − 1    2·log2(N)
    per-stage element              2×2 routing      ALU (reduce side)
                                   switch           FANOUT (bcast side)
                                   (4-in / 2-out)   (no routing)
    routing config / cycle         N · log2(N) b    0
    arbitrary permutation          YES              NO (binary tree only)
    sum / max / min reduce         NO (no ALU)      YES
    broadcast / fan-out            NO (route only)  YES
    ROW_SUM_EXP                    NO               YES
    in-tree sort                   partial          NO (need separate
                                   (with added                block)
                                   CMP_SWAP)

  ─── What our workload actually needs ─────────────────────────────────

    capability             needed?  Benes helps?
    ─────────────────────  ───────  ──────────────────────────────────
    reduce sum / max / min   YES    no  (binary tree is sufficient)
    broadcast / column exp   YES    no  (binary fan-out is sufficient)
    ROW_SUM_EXP              YES    no
    arbitrary permute        NO     —
    transpose                NO     done by TRegFile diagonal skew (§4)
    sort / merge_sort        maybe  no — bitonic uses FIXED butterfly,
                                    not Benes' configurable routing
    gather / scatter         NO     mask + ALU handle this
    FFT butterfly            NO     FFT also uses FIXED butterflies

  ─── Why Benes does NOT help with sort either ─────────────────────────

    Bitonic sort is a SEPARATE network class :
      - log²(N) / 2 stages, each with N/2 CMP_SWAP cells
      - per-stage wiring is FIXED (stride-1, stride-2, stride-4, ...
        butterfly patterns — same as FFT)
      - each cell does compare-and-conditional-swap (NOT pure routing)

    Pure Benes provides per-stage 2×2 ROUTING switches without ALU ;
    bitonic sort needs CMP_SWAP cells (= ALU + fixed wiring) at
    specific butterfly positions. These are DIFFERENT networks ;
    one does not subsume the other.

    Bitonic sort cell counts (rough, for context) :
      N =  128 (FP64) :  ~3.1 K CMP_SWAP cells
      N =  256 (FP32) :  ~4.0 K
      N =  512 (FP16) :  ~5.1 K
      N = 1024 (FP8 ) :  ~6.4 K

    A separate bitonic block can SHARE the CMP_SWAP cell library with
    the REDUCE tree (same 2-in / 1-out CMP_SWAP unit), but its wiring
    is its own butterfly, NOT a Benes routing fabric.

  ─── Recommended design AFTER dropping Benes routing ──────────────────

       Operand-A bus       Operand-B / Acc bus
            │                       │
            ▼                       ▼
   ┌──────────────────────────────────────────┐
   │  REDUCE  half  (binary fan-in tree)      │
   │   log2(N) stages, FIXED hardwired tree   │
   │   per-stage : N/2 ALU cells              │
   │   + INJECT  MUX  (format-tier truncate)  │
   │   + STOP-BIT MUX (row-width truncate)    │
   └────────────────┬─────────────────────────┘
                    ▼
          ┌──────────────────────┐
          │   ACC_SPILL latch    │
          └──────────────────────┘
                    ▼
   ┌──────────────────────────────────────────┐
   │  BROADCAST half (binary fan-out tree)    │
   │   log2(N) stages, FIXED hardwired tree   │
   │   per-stage : N/2 FANOUT cells           │
   │   + INJECT  MUX  (format-tier mirror)    │
   │   + STOP-BIT MUX (row-width mirror)      │
   └──────────────────────────────────────────┘

   *** OPTIONAL :  separate small bitonic-sort block,                ***
   ***             sharing the CMP_SWAP cell LIBRARY with REDUCE,    ***
   ***             but with its own FIXED butterfly wiring.          ***

  ─── Savings from eliminating Benes routing ───────────────────────────

    item                                          saving
    ────────────────────────────────────────────  ──────────────────────
    per-stage 2×2 routing MUXes                   ~ 100 K gates
    (~10 gates / byte × 1024 B × 10 stages)       ≈ 10–20 % of tree area
    critical-path MUX delay per stage             ~ 1–2 gate delays
                                                  per stage → integrated
                                                  tree latency drops
                                                  ~ 10–20 %
    microcode width per beat                      − 100 to − 200 b
                                                  (no routing-config
                                                  field needed)
    routing-config storage                        − ~ 2 KB latch / SRAM
    verification complexity                       1 order of magnitude
                                                  simpler (perm networks
                                                  are HARD to formally
                                                  verify ; binary trees
                                                  are trivial)
    conceptual clarity                            "back-to-back butterfly
                                                  binary tree" ≫
                                                  "Benes-like"

  ─── What we LOSE by eliminating Benes ────────────────────────────────

    Only one capability : in-tree arbitrary permutation. Our workload :
      • does not need it for compute  (reduce / broadcast / FMA /
        ROW_SUM_EXP)
      • handles transpose via TRegFile-4K diagonal skew  (vector4k_v2 §4)
      • handles sort via separate bitonic block          (cleaner anyway)
      • handles gather / scatter via mask + ALU          (rare in tile)
      • handles FFT via fixed-butterfly network          (not Benes)

    NET : pure simplification.  Strongly recommended.

  ─── Cleaner terminology ──────────────────────────────────────────────

    BEFORE :  "Benes-like shrink-then-expand network"
    AFTER  :  "back-to-back butterfly binary tree"
              or  "binary fan-in / fan-out tree"

    Same structure, but the new name correctly conveys :
      - it's a TREE (not a permutation network)
      - it has FIXED wiring (not configurable routing)
      - every internal node DOES COMPUTE (not just routes data)

══════════════════════════════════════════════════════════════════════════════
  OPERAND-A vs ACCUMULATOR PATH ON THE 1024 B TREE INPUT BUS
  (PARALLEL  vs  INTERLEAVED  ?)
══════════════════════════════════════════════════════════════════════════════

  ╔══════════════════════════════════════════════════════════════════╗
  ║  REVISION NOTE :                                                  ║
  ║  This section assumes the REDUCE tree has a 1024 B input bus     ║
  ║  carrying both operand A and operand B.  In the FINAL design     ║
  ║  (--unary-reduce), REDUCE-class instructions are unary and the   ║
  ║  tree input shrinks to 512 B (operand A only).  This whole       ║
  ║  parallel-vs-interleaved question is then MOOT.                  ║
  ║  See `python3 vector32_v2.py --unary-reduce`.                    ║
  ╚══════════════════════════════════════════════════════════════════╝

  Recommendation : PARALLEL `[A | B]`,  AND the accumulator feedback
                   does NOT live on the 1024 B input bus at all.

  ─── Three candidate layouts ───────────────────────────────────────────

    layout                  upper 512 B      L0 cell pairing rule
    ──────────────────────  ───────────────  ────────────────────────────
    PARALLEL  [A | B]       operand B / acc  cell k pairs lane (2k, 2k+1)
                                              ⇒ same-side pair (intra-A
                                                or intra-B)
    INTERLEAVED [A0,B0,...] interleaved B    cell k pairs (A_k, B_k)
                                              ⇒ forced cross-AB pair
    A-only + small acc port idle             same as PARALLEL ; acc
                                              feeds via separate path

  ─── Why PARALLEL wins ─────────────────────────────────────────────────

    1) Accumulator is SMALL.  For ROW_SUM_EXP :
         acc_bytes = R · E_out
           R=8,  FP32 out : 32 B    R=32, FP32 out : 128 B
           R=64, FP64 out : 512 B   (worst common case)
       Forcing it into a 512 B half (interleaved) wastes ~80 % of
       the upper-bus bandwidth.

    2) RFS_A and RFS_B are physically separate 4 KB RFs.  Their
       512 B tilelets naturally feed adjacent halves of the 1024 B
       input bus — PARALLEL is the zero-cost layout.

    3) PARALLEL allows INDEPENDENT reduce of A or B.  INTERLEAVED
       forces every L0 cell to pair (A_k, B_k), which is wrong for
       single-tile reduce.

    4) Binary cross-AB ops (e.g. dot product = Σ A_k · B_k) are
       handled by the per-lane ALU array UPSTREAM of the tree
       (vector4k_v2.md §5 Stage-B core), NOT inside the tree's L0.
       The tree only sees the per-lane ALU's already-paired output.

    5) Accumulator feedback uses a SEPARATE small path :
       a K-wide RMW ALU sitting at the spill latch, NOT on the
       1024 B main input bus.

  ─── Recommended architecture ──────────────────────────────────────────

    [Operand-A tilelet 512 B]         [Operand-B tilelet 512 B  or idle]
              │                                       │
              ▼                                       ▼
       lanes 0..511                            lanes 512..1023
              │                                       │
              └────────────────┬──────────────────────┘
                               ▼
    ┌──────────────────────────────────────────────────────────┐
    │   1024 B REDUCE TREE  (back-to-back butterfly binary)    │
    │                                                          │
    │   L0 cell k pairs lanes (2k, 2k+1)  — DEFAULT same-side  │
    │     (intra-A in lower half,  intra-B in upper half)      │
    │                                                          │
    │   per-cell `pair_mode` bit can switch to cross-AB :      │
    │     cell k pairs lane k with lane k+512   (rarely used)  │
    │     cost : ~5 gates / cell × 512 cells = 2.5 K gates     │
    └────────────────┬─────────────────────────────────────────┘
                     │
                     ▼   K partials at chosen stop level
    ┌──────────────────────────────────────────┐
    │   K-wide  RMW ALU  +  ACC_SPILL latch    │  ← acc[K] from
    │   acc[r] += partial[r]  (in-place add)   │     RFS_B (small
    │   K · E_out bytes wide  (≤ 1 KB)         │     read, off the
    │                                          │     main bus)
    └────────────────┬─────────────────────────┘
                     │
                     ▼   acc[K] (small write back)
              RFS_B[acc_slot : acc_slot+K·E_out]

  ─── Why the acc path stays OFF the 1024 B bus ────────────────────────

    Putting the acc on the main bus would mean :
      • reading 512 B of RFS_B every beat just to use ~32 B of acc
      • routing acc through 9 stages of reduce tree to reach the K
        partials — but acc[r] is added at ONE place (the K-wide RMW)
        and doesn't need to traverse all 9 stages
      • burning power on 480 B of don't-care bus traffic per beat

    Keeping acc off the main bus (separate K-wide port + RMW ALU)
    saves :
      • ~80 % power on the upper-half bus (it can be clock-gated for
        unary reduce ops where operand B isn't used)
      • a wide cross-tree wire (acc would otherwise need to fan into
        whichever stop level holds the K partials)
      • clarifies the architecture : the tree REDUCES, the K-wide
        ALU ACCUMULATES — separation of concerns

  ─── When INTERLEAVED actually makes sense (rare) ──────────────────────

    Only in two niche cases :

      • DOT PRODUCT inside a single 1024 B tree pass :
          partial = Σ_k A_k · B_k  ,  reduce in one beat
        L0 cells doing (A_k · B_k) need (A_k, B_k) adjacent →
        INTERLEAVED helps. But our design routes the multiply through
        the per-lane ALU first ; the tree only sums afterwards.

      • SORTED-ARRAY MERGE (merge two sorted halves) where each L0
        cell does CMP_SWAP between A_k and B_k :
        same situation — the bitonic merge stage's wiring is what
        does this, NOT the reduce tree.

    Conclusion : INTERLEAVED's niche is covered by separate hardware
    (per-lane ALU upstream OR bitonic sort block). The reduce tree
    itself stays PARALLEL.

══════════════════════════════════════════════════════════════════════════════
  ACCUMULATOR FLIP-FLOPS  (3 questions)
    Q1 : share with operand-B staging RF, or dedicated FFs ?
    Q2 : feedback path  ── local at the RMW, or back to tree front ?
    Q3 : worst-case (shape × format) acc byte requirement ?
══════════════════════════════════════════════════════════════════════════════

  ╔══════════════════════════════════════════════════════════════════╗
  ║  REVISION NOTE :                                                  ║
  ║  Q1's recommendation (SEPARATE acc FFs) is SUPERSEDED by the      ║
  ║  --unary-reduce design, which makes REDUCE instructions unary,    ║
  ║  resolves the port conflicts at the instruction-class level, and  ║
  ║  reuses RFS_B as the acc.  Q2 (LOCAL feedback) and Q3 (sizing     ║
  ║  formulas) remain valid.                                          ║
  ║  See `python3 vector32_v2.py --unary-reduce` for the FINAL        ║
  ║  recommendation.                                                  ║
  ╚══════════════════════════════════════════════════════════════════╝

  ━━━ Q1 :  SEPARATE  vs  SHARE-with-RFS_B  ━━━━━━━━━━━━━━━━━━━━━━━━━━━━

  Recommendation : SEPARATE, dedicated small acc-FF bank.
                   Do NOT repurpose RFS_B as accumulator.

  ─── Comparison ────────────────────────────────────────────────────────

    issue                       SHARED  (reuse RFS_B)        SEPARATE  (dedicated)
    ─────────────────────────   ───────────────────────────  ─────────────────────────
    Read-port conflict          RFS_B can't serve binary-    acc has its own port,
                                op operand B AND RMW         no contention
                                acc-fetch in same cycle

    3-src FMA  (D = A·B + C)    if C-operand routes from     acc independent →
                                acc, RFS_B port is busy →    same-cycle acc read +
                                stall                         RFS_B read possible

    Op-to-op pipelining         op_n's acc result lives in   acc decoupled ; op_{n+1}
                                RFS_B ; op_{n+1} that wants  is free to use RFS_B
                                RFS_B as operand B must
                                first move it out → bubble

    Acc footprint               steals up to 1 KB of RFS_B   right-sized (2 KB max,
                                even though typical acc      see Q3) ; no waste in
                                < 512 B → wastes RFS_B       RFS_B

    Verification surface        one RF, two semantics ;      separation of concerns ;
                                bugs cross-pollute           clean boundaries

    Area                        −16 K FFs (saved)            +16 K FFs (~50 K gates
                                                              in 28 nm) for 2 KB acc

  Trade-off : we PAY ~50 K gates to GAIN port-conflict-free pipelining,
  3-src-FMA support, and a clean RFS_B that is ALWAYS the operand-B
  staging register. The 50 K gates is < 0.05 % of the VEC die, well
  worth the architectural simplification.

  ━━━ Q2 :  Feedback path ── LOCAL or back to TREE FRONT ?  ━━━━━━━━━━━━

  Recommendation : LOCAL feedback only.  acc never goes back to L0 of
                   the reduce tree.

  ─── Comparison ────────────────────────────────────────────────────────

    path                LOCAL feedback              FRONT feedback
                        (recommended)               (rejected)
    ─────────────────   ─────────────────────────   ─────────────────────────
    where it loops      tree-exit → K-wide RMW      tree-exit → 4 KB bus →
                        ALU → acc FF → RMW          back to tree L0 input
                                                    (upper 512 B half)

    operations that     reduce (sum/max/min)        per-element accumulation
    benefit             ROW_SUM_EXP                 (e.g. dense GEMM tile-acc)
                        COL_SUM_EXP                 — but those use the per-lane
                        ROW_MAX, COL_MIN, ...       FMA + RFA_A/B already !

    cost                K · E_out wide ALU +        4 KB feedback bus + steals
                        1 short feedback wire       upper 512 B input port +
                                                    every beat goes through 9
                                                    extra tree levels for what
                                                    is a single ADD

    gates               ~ 1 K                       ~50 K + 1 extra pipe stage

  Why : in ROW_SUM_EXP the acc is combined with the new partials ONLY
  ONCE per beat (K-wide ADD).  Routing acc back through 9 reduce-tree
  levels just to add it would be wasted travel.  Per-element GEMM-class
  accumulation belongs in the per-lane FMA core (vector4k_v2.md §5.5),
  not in the reduce-tree pipeline.

  ─── Architecture diagram (LOCAL feedback) ─────────────────────────────

             ┌────────────────────────────────────┐
             │   1024 B REDUCE TREE                │
             │   (operand A on lower half,         │
             │    operand B / idle on upper half)  │
             └─────────────────┬───────────────────┘
                               │  K · E_out  partials
                               ▼
    ┌──────────────────────────────────────────────┐
    │  K-WIDE  RMW  ALU  (ADD / MAX / MIN)         │
    │                                              │
    │       acc_FF[r] := acc_FF[r]  ⊕  partial[r]  │ ◀──┐
    └──────────────────────┬───────────────────────┘    │ same-cycle
                           │                            │ feedback
                           ▼                            │
            ┌───────────────────────────────────┐       │
            │   ACC FF bank   (2 KB max)         │ ──────┘
            │   addressed by acc_slot[r]         │
            └─────────────────┬─────────────────┘
                              │  on broadcast phase :
                              ▼
            ┌───────────────────────────────────┐
            │   ACC_SPILL latch (snapshot)       │
            │   feeds BROADCAST tree input       │
            └───────────────────────────────────┘

  ━━━ Q3 :  Worst-case  acc-byte  requirement  ━━━━━━━━━━━━━━━━━━━━━━━━━

  ROW_SUM_EXP acc formula :

      acc_bytes = R · E_out                    (one slot per row)
                = (4096 / (C · E_in)) · E_out  (using R·C·E_in = 4096)

      Same-precision out  (E_out = E_in)  :   acc_bytes = 4096 / C
      Double-precision out (E_out = 2·E_in) : acc_bytes = 8192 / C

  Note : acc size depends only on C (reduce dim) and double-width flag.
         It is INDEPENDENT of E_in.

  ─── ROW_SUM_EXP : acc as a function of minimum supported C ────────────

    C_min   same-prec acc_max   double-prec acc_max   remark
    ─────   ─────────────────   ───────────────────   ─────────────────────────
        2          2048 B              4096 B          degenerate (sum of 2)
        4          1024 B              2048 B          short reduce
        8           512 B              1024 B          ← reasonable lower bound
       16           256 B               512 B          common min
       64            64 B               128 B          typical head-dim
      256            16 B                32 B          large head

  ─── Other reduce-class ops (worst case at 4 KB tile) ──────────────────

    operation                   acc formula                worst-case
    ─────────────────────────   ────────────────────────   ──────────
    ROW_SUM_EXP  / ROW_MAX      R · E_out                  ~2 KB
    COL_SUM_EXP  / COL_MAX      C · E_out                  ~2 KB
    FULL_REDUCE (whole tile)    E_out                      8 B
    ARGMAX per row              R · (E_out + idx_bytes)    ~2.5 KB
    TOP-K per row (K=8)         R · K · E_out             ⚠  ~16 KB

    ⚠ TOP-K is an outlier — it should NOT share the reduce-tree acc.
       Use a dedicated K-element scratch attached to the SORT network.
       Otherwise the reduce-tree acc would be 8× over-provisioned.

  ─── Sizing recommendation ─────────────────────────────────────────────

    target                                       acc-FF size
    ──────────────────────────────────────────   ───────────
    cheap : same-prec only,  C ≥ 8                 1 KB
    standard : double-prec,  C ≥ 8                 1 KB  (out_max = 1024)
    standard : double-prec,  C ≥ 4                 2 KB  ← RECOMMENDED
    permissive : double-prec, C ≥ 2                4 KB

    RECOMMENDED : 2 KB (16 K FF, ~50 K gates in 28 nm)
      ✓ covers ROW_SUM_EXP and COL_SUM_EXP up to R, C ≤ 256 in
        double-precision mode
      ✓ covers ARGMAX per row up to R = 256
      ✓ leaves headroom for narrower C (down to C = 4 in double-prec)
      ✗ does NOT try to satisfy TOP-K (handled separately)
      ✗ does NOT try to satisfy degenerate C = 2 (cheap to outlaw
        in the spec — there's no real workload)

  ─── Cost-of-doubling table (for design space exploration) ─────────────

    acc size   FFs (×16 b)   cell area (28 nm)   coverage gained
    ────────   ───────────   ─────────────────   ─────────────────────────
    1 KB       8192          ~25 K gates         standard up to C ≥ 16
    2 KB       16384         ~50 K gates         RECOMMENDED  ← best knee
    4 KB       32768         ~100 K gates        covers C = 2 (rare)
    8 KB       65536         ~200 K gates        no further coverage benefit

══════════════════════════════════════════════════════════════════════════════
  FORMAT-TIER INJECTION + STOP-BIT TRUNCATION  (multi-format design)
══════════════════════════════════════════════════════════════════════════════

  Q : how does ONE physical tree handle FP8 (N=1024), FP16 (N=512),
      FP32 (N=256), FP64 (N=128) inputs AND arbitrary row widths C ?

  A : two ORTHOGONAL truncation knobs, applied at opposite ends of
      the tree :

      ┌────────────────┬────────────────────┬────────────────────────┐
      │                │  TOP truncation    │  BOTTOM truncation     │
      ├────────────────┼────────────────────┼────────────────────────┤
      │ trigger        │  wider E_in        │  narrower row width C  │
      │ skip which lvl │  first  log2(E_in) │  last  log2(N_max) −   │
      │                │  stages (leaf side)│  log2(elems/subtree)   │
      │                │                    │  stages (root side)    │
      │ mechanism      │  per-stage         │  per-stage             │
      │                │  INJECT MUX        │  STOP-BIT MUX          │
      │ example        │  FP32 skips L0,L1  │  (32×32 FP32) → 4 sub- │
      │                │                    │  trees stop at L7      │
      └────────────────┴────────────────────┴────────────────────────┘

  ─── TOP truncation : format-tier injection ────────────────────────────

       1024 B operand-A bus
            │
            ├──►[INJECT_L0 MUX]──► L0  (1024 × 1B  FP8  elements)   ← FP8
            │                          │  stage 0 :
            │                          ▼  512 × (1B+1B → 2B promote)
            │                          │
            ├──►[INJECT_L1 MUX]──► L1  ( 512 × 2B  FP16 elements)   ← FP16
            │                          │  stage 1 :
            │                          ▼  256 × (2B+2B → 4B promote)
            │                          │
            ├──►[INJECT_L2 MUX]──► L2  ( 256 × 4B  FP32 elements)   ← FP32
            │                          │  stage 2 :
            │                          ▼  128 × (4B+4B → 8B promote)
            │                          │
            ├──►[INJECT_L3 MUX]──► L3  ( 128 × 8B  FP64 elements)   ← FP64
            │                          │  stage 3..9 : 8B+8B → 8B
            │                          ▼  (cap at FP64 width)
            │                         ...
            │                          ▼  ROOT

    Key design points :

      1) Per-stage INJECT MUX : 2:1 select { cascade_from_prev_stage,
                                              fresh_bus[level_k] }.
         Select = 1 ⇒ this level is the input ; all earlier stages
         are clock-gated off.

      2) ALU width doubles per stage (1B → 2B → 4B → 8B → cap).  This
         geometric progression EXACTLY MATCHES the IEEE-754 format
         widths {FP8, FP16, FP32, FP64}, so every format has a
         natural injection point.

      3) Width promotion is FREE.  FP8 input naturally widens to
         FP16 after stage 0, FP32 after stage 1, FP64 after stage 2.
         Optional cast/round unit between stages can cap the width
         (e.g. cap FP32 output : FP8 → FP32 in 2 stages, then stay
         FP32 for the remaining stages).

      4) Lane mapping is contiguous : bytes [i·E_in, (i+1)·E_in)
         belong to element i.  Stage-k pair-adders always combine
         element 2i with element 2i+1, regardless of E_in.

      5) Input bus fan-out cost : the 1024 B operand-A bus must
         drive ALL 4 inject points → +3 buffer stages between
         injects (≈1 K gates, negligible vs ALU area).

      6) Latency varies with format :
           FP8 : 10 stages,  FP16 : 9,  FP32 : 8,  FP64 : 7
         If uniform latency is desired, NOP pipeline-regs fill the
         skipped slots (no ALU, just registers).

  ─── BOTTOM truncation : per-row stop-bit ──────────────────────────────

    ... ─►─ stage k ALU ─►─ pipeline reg ─►──┬─►─ STOP-BIT MUX ─►─
                                              │             │
                                              │             ▼
                                              │     ACC_SPILL[k_subtree]
                                              │
                                              └─►─ to stage k+1

    Key design points :

      1) Per-stage STOP-BIT MUX : 2:1 select { cascade_to_next_stage,
                                                 route_to_spill_slot }.

      2) Stop-level = inject_level + ⌈log2(elems_per_subtree)⌉
         Examples :
           (32,32,FP32) : inject L2, 32 elems/row, stop at L7
           (64,64,FP8 ) : inject L0, 64 elems/row, stop at L6

      3) K parallel sub-trees stop at the SAME stage simultaneously
         (one beat = one stop level), each routes to its own slot
         in the K-wide ACC_SPILL latch.

      4) Stages past the stop level are clock-gated (saves power on
         small-row beats).

  ─── Per-stage cell architecture (combined INJECT + ALU + STOP) ────────

        Fresh input bus slice          Cascade from stage k−1
                │                              │
                ▼                              ▼
          ┌─────────────────────────────────────────┐
          │   INJECT MUX  (TOP truncation)          │  inject_here[k] (1b)
          └─────────────────────────────────────────┘
                              │
                              ▼
          ┌─────────────────────────────────────────┐
          │   E_k-wide  ALU  +  optional CAST       │  opcode      (3b)
          │   (ADD | CMP_SWAP | PASS | MUX)         │  promote_bit (1b)
          └─────────────────────────────────────────┘
                              │
                              ▼
                       pipeline register
                              │
                              ▼
          ┌─────────────────────────────────────────┐
          │   STOP-BIT MUX  (BOTTOM truncation)     │  stop_here[k]   (1b)
          └──────┬─────────────────────────┬────────┘
                 │                         │
                 ▼ cascade                 ▼ stop
           next stage              ACC_SPILL[k_subtree_id]

    Per-stage microcode bits : inject(1) + opcode(3) + promote(1) +
                               stop(1) = 6 bits / stage
    Total / beat : 10 stages × 6 b = 60 b microcode word
                   (fits inside the §5.4 beat-word easily)

  ─── Per-stage ALU width and format-injection table ────────────────────

    stage   level→level+1   N_elem    E_k width   ALU op        native fmt
    ─────   ─────────────   ──────    ─────────   ───────────   ──────────
    inject     → L0          1024       1 B         —           ★ FP8
       0    L0 → L1     1024 → 512    1B → 2B    1B+1B→2B promo  cascade
    inject     → L1           512       2 B         —           ★ FP16
       1    L1 → L2      512 → 256    2B → 4B    2B+2B→4B promo  cascade
    inject     → L2           256       4 B         —           ★ FP32
       2    L2 → L3      256 → 128    4B → 8B    4B+4B→8B promo  cascade
    inject     → L3           128       8 B         —           ★ FP64
       3    L3 → L4      128 →  64    8B (cap)   8B+8B→8B        cascade
       4    L4 → L5       64 →  32    8B         8B+8B→8B        cascade
       5    L5 → L6       32 →  16    8B         8B+8B→8B        cascade
       6    L6 → L7       16 →   8    8B         8B+8B→8B        cascade
       7    L7 → L8        8 →   4    8B         8B+8B→8B        cascade
       8    L8 → L9        4 →   2    8B         8B+8B→8B        cascade
       9    L9 → L10       2 →   1    8B         8B+8B→8B        ROOT

  ─── Worked examples (4 formats × multiple row widths) ─────────────────

    input tile         inject  subtree   stop   #skipTOP  #skipBOT  active
    ─────────────────  ──────  ───────   ────   ────────  ────────  ──────
    (1,  1024, FP8 )   L0      128 elem  L7        0         3        7
    (1,  1024, FP32)   L2      128 elem  L9        2         1        7
    (8,   128, FP32)   L2      128 elem  L9        2         1        7
    (32,   32, FP32)   L2       32 elem  L7        2         3        5
    (64,   64, FP8 )   L0       64 elem  L6        0         4        6
    (8,   256, FP16)   L1      256 elem  L9        1         1        8
    (8,    64, FP64)   L3       64 elem  L9        3         1        6
    (256,   4, FP32)   L2        4 elem  L4        2         6        2  ← extreme

    The last case ((256, 4, FP32) — 4-elem rows) uses only 2 of 10
    pipeline stages : 80 % of the tree is clock-gated this beat.

  ─── Area cost vs alternatives ─────────────────────────────────────────

    scheme                                         relative ALU area
    ─────────────────────────────────────────      ─────────────────
    4 independent trees (one per FP{8,16,32,64})         4.0 ×
    1 byte-granular tree + carry-chain configure         ~1.3 ×  *slow
    1 format-tier tree (this design)                     ~1.5 ×
    1 single-format tree (FP32 only, no flexibility)      1.0 ×

    Why 1.5× and not 1.0× for the format-tier scheme :
      • per-stage ALU width doubles (1B → 8B), but element count
        halves, so the per-stage byte count is ~constant ≈ 1024 B
      • 10 stages × ~1024 B of ALU logic ≈ 10 KB of arithmetic,
        ≈ 1.5× a single FP32-wide tree (which is 8 stages × 1024 B)
      • the +50 % buys :  4 formats × any row width support,
                         + free width promotion for ROW_SUM_EXP-style
                           narrow→wide accumulation,
                         + reuse for sort / merge_sort (just opcodes).

══════════════════════════════════════════════════════════════════════════════
  TOP-K and ARG-MAX  :  SORT  or  REDUCE  ?
══════════════════════════════════════════════════════════════════════════════

  Q : should ARG-MAX and TOP-K go through the SORT network instead
      of being treated as REDUCE-class operations ?

  A : depends per-op.  Most are REDUCE ; only large-K and MEDIAN are
      genuinely SORT.  Mapping :

    op                        nature                        recommended impl
    ───────────────────────   ──────────────────────────    ────────────────────
    ARG-MAX  per row/col      reduce (associative)          REDUCE  (augmented
                              + carry index                  CMP_SWAP cell)
    TOP-K, K = 1              ≡ ARG-MAX                     REDUCE
    TOP-K, K ≤ 4              K-pass channeled reduce       REDUCE  (channeled)
    TOP-K, K > 4              partial sort                  SORT  (if hw exists)
    MEDIAN                    sort + index N/2              SORT  (mandatory)
    HISTOGRAM                 scatter + atomic-add          REDUCE + atomics

  ─── Why ARG-MAX is naturally a REDUCE ─────────────────────────────────

    ARG-MAX is associative :   max(max(a, b), c) = max(a, max(b, c))
    So it maps to a binary REDUCE tree.  Each CMP_SWAP cell propagates
    a (value, partial-index) pair upward — value selects, index follows.

            level L+1
             ┌─────────────────────┐
             │  MAX_BY_VALUE        │  → outputs (max_val, idx_path)
             └──────────┬──────────┘
                ┌───────┴───────┐
        (val_L, idx_L)   (val_R, idx_R)        ← from level L cells

    Cell cost : value width E_out  +  partial-index width log₂(subtree)
    Top-of-tree : full index width log₂(C) — typically 2 B for C ≤ 65 K.

    Acc bytes per row :  E_out + log₂(C)/8  ≈  E_out + 2 B
    Total acc :  R · (E_out + 2)  — at most ~2.5 KB for R=256, FP64.

  ─── Why ARG-MAX through SORT is wasteful ──────────────────────────────

    For row of N elements :

                          REDUCE-tree    SORT (bitonic)    ratio
                          (ARG-MAX)      (full sort)
    ────────────────────  ────────────   ──────────────    ────────
    cells (N=1024)        ~5 K           ~28 K             5.6× more
    pipe stages           10             55                5.5× longer
    ALU width per cell    E_out + idx    E_out + idx       same

    Sorting an entire row just to read its last element is the same
    mistake as bubble-sort to find the max — we throw away ordering
    information we never needed.

  ─── TOP-K : channeled REDUCE  vs  SORT  ───────────────────────────────

    Channeled REDUCE (K passes through the ARG-MAX reduce tree):

        for k in 0 .. K-1 :
            (val[k], idx[k]) = ROW_ARGMAX(input WHERE mask[*])
            mask[idx[k]] = 0       # zero out winner for next pass
        result = top-K (val, idx) pairs per row

        latency : K × tree_depth ≈ K · 10 cycles  (for N=1024)
        acc     : R · K · (E_out + 2) bytes

    Bitonic SORT + truncate :

        sorted = ROW_BITONIC_SORT(input)
        result = sorted[:K]   (or sorted[N-K:])

        latency : log₂²(N)/2 stages = ~55 cycles for N=1024
        acc     : K · (E_out + 2) per row, in sort-network scratch
        cells   : 28 K (for the full sort net, paid once)

  ─── Crossover analysis (N=1024 row) ───────────────────────────────────

       K     channeled-reduce    sort        winner       ratio
              cycles              cycles
       ───   ─────────────────   ─────────   ──────────   ─────
        1            10            55        REDUCE       5.5×
        2            20            55        REDUCE       2.8×
        4            40            55        REDUCE       1.4×
        5            50            55        ≈ tie        crossover
        8            80            55        SORT         1.5×
       16           160            55        SORT         2.9×
       32           320            55        SORT         5.8×

    Caveat : SORT incurs a one-time ~28 K cell cost. If the workload
    NEVER uses MEDIAN or large-K TOP-K, that 28 K is dead silicon ;
    in that case force all TOP-K through channeled reduce (cap K ≤ 4
    in the spec) and skip the sort network entirely.

  ─── Implication for acc-FF sizing (Q3 from previous section) ──────────

    If we OFFLOAD large-K TOP-K and MEDIAN to the SORT network, the
    REDUCE-tree's acc footprint stays tame :

       op going through REDUCE         acc formula            worst case
       ────────────────────────────    ───────────────────    ──────────
       ROW_SUM_EXP / ROW_MAX           R · E_out              ~2 KB
       ARG-MAX per row                 R · (E_out + 2)        ~2.5 KB
       TOP-K  (K ≤ 4) channeled        R · K · (E_out + 2)    ~2.5 KB
                                       (R=64 K=4 FP64)

       op going through SORT           acc formula            worst case
       ────────────────────────────    ───────────────────    ──────────
       TOP-K  (K > 4)                  K · (E_out + 2)        ~0.3 KB
                                       per row, in sort scratch
       MEDIAN                          E_out per row          ~2 KB
                                       (in sort network)

    The 2 KB REDUCE-acc recommendation from Q3 STILL HOLDS when
    TOP-K large is routed to SORT. Otherwise it would need to grow
    to ~16 KB to absorb K=8 TOP-K through channeled reduce — strong
    argument for keeping the SORT network if any large-K op is needed.

  ─── Architecture decision tree ────────────────────────────────────────

    workload requires :              build SORT net ?    cap on TOP-K's K ?
    ─────────────────────────────    ────────────────    ──────────────────
    only sum / max / min / argmax    NO                  K ≤ 4 (channeled)
    + small-K TOP-K (K ≤ 4)          NO                  K = 4
    + medium-K TOP-K (K=8..32)       YES (or accept       K ≤ 32 (sort)
                                     slow channeled)
    + MEDIAN / sample / sort         YES                  no cap

  ─── Why SORT and REDUCE are truly complementary, not redundant ────────

    REDUCE answers : 'what is THE winner ?'  (1 element + maybe its idx)
    SORT   answers : 'what are ALL elements in order ?' (N positions)

    ARG-MAX is 1-element-with-idx → REDUCE.
    Large-K TOP-K is K-positions-with-relative-order → SORT.
    They are different problem classes, served by different hw.

══════════════════════════════════════════════════════════════════════════════
  SORT NETWORK  (Batcher bitonic)  — shared across formats & shapes
══════════════════════════════════════════════════════════════════════════════

  Q : how do we sort tile data inside the VEC unit, with ONE physical
      network supporting all (shape × format) combinations ?

  ─── Why Batcher bitonic ───────────────────────────────────────────────

    algorithm           network depth     cells           remarks
    ─────────────────   ───────────────   ─────────────   ─────────────────
    bubble sort         N                 N² / 2          too slow / too big
    Batcher bitonic     k·(k+1) / 2       N·k·(k+1) / 4   *data-independent
                        (k = log₂ N)                       latency, FIXED
                                                           butterfly wiring,
                                                           single CMP_SWAP
                                                           cell type
    AKS sorting         O(log N)          O(N log N)      theoretical, huge
                                                           constants, never
                                                           built

    Bitonic IS the de-facto industry standard for hardware sort.

  ─── N=8 example schedule  (6 stages × 4 cells = 24 CMP_SWAP cells) ────

    Phase   Stage   byte-d   cells (a↑b = ascending, a↓b = descending)
    ─────   ─────   ──────   ────────────────────────────────────────────
    BUILD    S1       1      (0↑1)  (3↓2)  (4↑5)  (7↓6)
                             ── form 4 alternating ↑↓ pairs of 2 ──
             S2       2      (0↑2)  (1↑3)  (6↓4)  (7↓5)
                             ── merge into 4-bitonic groups ──
             S3       1      (0↑1)  (2↑3)  (5↓4)  (7↓6)
                             ── clean up within each 4-group ──
    MERGE    S4       4      (0↑4)  (1↑5)  (2↑6)  (3↑7)
                             ── cross-half merge into 8-bitonic ──
             S5       2      (0↑2)  (1↑3)  (4↑6)  (5↑7)
                             ── halve and merge ──
             S6       1      (0↑1)  (2↑3)  (4↑5)  (6↑7)
                             ── final stride-1 clean up ──

    Output : i0 ≤ i1 ≤ i2 ≤ i3 ≤ i4 ≤ i5 ≤ i6 ≤ i7

  ─── One-stage detail : how a single stage looks (S1, d=1) ─────────────

    Each stage is a column of N/2 CMP_SWAP cells operating in parallel.
    For N=8 stage S1 (distance 1, alternating ↑↓ direction) :

              line 0 ──────●───────
                           │ ↑   <- cell (0,1) ascending : if line0 > line1, swap
              line 1 ──────●───────
              line 2 ──────●───────
                           │ ↓   <- cell (2,3) descending: if line2 < line3, swap
              line 3 ──────●───────
              line 4 ──────●───────
                           │ ↑   <- cell (4,5) ascending
              line 5 ──────●───────
              line 6 ──────●───────
                           │ ↓   <- cell (6,7) descending
              line 7 ──────●───────

    All 4 cells fire in the same cycle ; output of stage S1 then feeds
    stage S2's input. Other stages have the same shape — only the cell
    pairing (a, b) and direction change. See the table above for those.

  ─── Recursive block structure (Batcher's BUILD-then-MERGE) ────────────

    For N=8, the network unrolls from this recursion :

    ┌─────────────────────────────────────────────────────────┐
    │ N=8  bitonic_sort(0..7, ↑)                               │
    │                                                         │
    │   ┌─────────────────────┐  ┌─────────────────────┐      │
    │   │ N=4 sort(0..3, ↑)    │  │ N=4 sort(4..7, ↓)    │      │  PHASE A
    │   │                     │  │                     │      │  BUILD
    │   │ ┌───┐ ┌───┐         │  │ ┌───┐ ┌───┐         │      │  bitonic
    │   │ │ ↑ │ │ ↓ │ <-S1    │  │ │ ↑ │ │ ↓ │ <-S1    │      │  seqs
    │   │ │0,1│ │2,3│         │  │ │4,5│ │6,7│         │      │
    │   │ └───┘ └───┘         │  │ └───┘ └───┘         │      │
    │   │ merge(0..3, ↑) <-S2 │  │ merge(4..7, ↓) <-S2 │      │
    │   │       <-S3          │  │       <-S3          │      │
    │   └─────────────────────┘  └─────────────────────┘      │
    │                                                         │
    │   ┌──────────────────────────────────────────────┐      │
    │   │ bitonic_merge(0..7, ↑)                        │      │  PHASE B
    │   │   S4 d=4 : cross-half (0,4)(1,5)(2,6)(3,7)    │      │  MERGE
    │   │   S5 d=2 : (0,2)(1,3)(4,6)(5,7)               │      │  into
    │   │   S6 d=1 : (0,1)(2,3)(4,5)(6,7)               │      │  sorted
    │   └──────────────────────────────────────────────┘      │
    └─────────────────────────────────────────────────────────┘

    For N=1024 the recursion has 10 levels ; total stages = 55.
    The cell PAIRINGS at each stage are FIXED by the recursion ;
    no run-time routing is needed (no Benes, no MUX-permutation).

  ─── Stage / cell counts for the 4 native sizes ────────────────────────

       N      format   stages  cells/stage   total cells
       ────   ──────   ──────  ───────────   ───────────
       1024   FP8        55      512           28160
        512   FP16       45      256           11520
        256   FP32       36      128            4608
        128   FP64       28       64            1792

    Build the LARGEST one (N=1024, 28 K cells) physically. Smaller
    formats reuse it via TOP truncation (skip leading stages).

  ─── TOP truncation : format-tier injection (skip front stages) ────────

    For a wider format E_in, byte-distance < E_in stages are MEANINGLESS
    (they would compare bytes inside the same element). Skip those
    stages — clock-gate them off, route input directly to the first
    meaningful stage.

       format   element     skip stages with     #stages   #stages
                width E_in  byte-d ∈             skipped   used
       ──────   ──────────  ──────────────────   ───────   ───────
       FP8         1 B      (none)                 0         55
       FP16        2 B      d=1                   10         45
       FP32        4 B      d=1, d=2              19         36
       FP64        8 B      d=1, d=2, d=4         27         28

    (For N=1024 schedule, 10 phases each contain one d=1 stage,
     9 phases contain one d=2 stage, 8 phases contain one d=4 stage.)

  ─── BOTTOM truncation : sub-tile parallel sorts (skip merge phases) ──

    To sort K parallel sub-arrays of N/K elements each, use only the
    first  log₂(N/K)  phases ; the later phases that MERGE across the
    K sub-arrays are skipped.

       use case (N=1024 worst)     sub-sort   m=log₂   stages used
                                   shape      sub-N    = m(m+1)/2
       ─────────────────────────   ────────   ──────   ───────────
       full tile sort              1 × 1024     10          55
       8 rows of 128 each          8 × 128       7          28
       64 rows of 16 each          64 × 16       4          10
       1024 rows of 1 (trivial)    1024 × 1      0           0

    Combined TOP + BOTTOM example :  (8 rows × 32 FP32 elems each)
       N=256 (FP32) physical schedule : 36 stages
       BOTTOM cap to 32-elem sub-sort : m=5 → 5·6/2 = 15 stages
       Plus skip d=1, d=2 (TOP) within those 15 : actually we only
       count the stages used in the m=5 sub-network with the wider
       format ⇒ ~14 stages used

  ─── Recommended architecture (one shared sort block) ──────────────────

       1024 B input bus  (from RFS_A or any RFS)
              │
              ▼
    ┌─────────────────────────────────────────────────────────┐
    │   FORMAT-TIER INJECT MUX  (TOP truncation)               │
    │   ── select start-stage based on E_in ──                 │
    └────────────────┬────────────────────────────────────────┘
                     │
                     ▼
    ┌─────────────────────────────────────────────────────────┐
    │   BITONIC SORT NETWORK  (28 K CMP_SWAP cells, N_max=1024)│
    │   55 stages × 512 cells, FIXED butterfly wiring          │
    │   per-cell : 8 B max-width CMP_SWAP_HI/LO + dir bit      │
    │   per-stage : 1 b stop-bit (BOTTOM truncation)           │
    └────────────────┬────────────────────────────────────────┘
                     │
                     ▼
       1024 B output bus  (sorted, back to RFS_A or RFS_B)

  ─── What the sort block SHARES with the reduce/broadcast tree ─────────

    resource                       reduce tree   sort net   shared ?
    ────────────────────────────   ───────────   ────────   ──────────
    CMP_SWAP cell library          ✓ (max/min)   ✓ (main)   ★ same lib
    ADD cell library               ✓             ✗
    FANOUT cell                    ✓             ✗
    INJECT MUX  (format-tier)      ✓             ✓          ★ same ctrl
    STOP-BIT MUX (shape-tier)      ✓             ✓          ★ same ctrl
    Per-stage wiring topology      binary tree   bitonic    NO — must
                                                              be 2 blocks
    Microcode op decoder           ✓             ✓          ★ same op
                                                              format

    Conclusion : 2 PHYSICAL networks (cannot fold one into the other),
    but they share the cell library, control electronics, and microcode
    encoding. A single ALU-cell macro is reused across both.

  ─── Total area estimate for combined REDUCE + BROADCAST + SORT ────────

    block                                cells     gates
    ──────────────────────────────────   ───────   ─────────
    REDUCE/BROADCAST tree (this design)   ~12 K    ~600 K
    SORT network (FP8 worst case)         ~28 K    ~1.4 M
    Shared control + spill latches         —       ~200 K
    ──────────────────────────────────   ───────   ─────────
    TOTAL                                 ~40 K    ~2.2 M gates

    Reference (vector4k_v2 §5 Stage-B core) :
      128 FMA lanes × 10 K gates each  ≈  1.28 M gates

    ⇒  sort network alone ≈ 1× FMA-core area.  Worth adding only if
        the workload uses sort (top-K attention, median, sparse-
        intersect, etc.) ; otherwise omit and save ~1.4 M gates.

══════════════════════════════════════════════════════════════════════════════
  REVISED  ACCUMULATOR + TREE  DESIGN
  Unary REDUCE  +  RFS_B-as-acc  +  half-width (512 B) tree
══════════════════════════════════════════════════════════════════════════════

  This section SUPERSEDES parts of --accumulator-design (Q1) and
  --operand-layout. The earlier sections analysed the case where the
  REDUCE instruction class is binary (consumes operand A AND B); here
  we revisit the design under the simpler hypothesis :

        REDUCE-class instructions are UNARY (operand A only).

  ─── Key observation that triggers the revision ────────────────────────

    Worst-case accumulator footprint EQUALS operand-B RF size :

        ROW_SUM_EXP, C=2, E_in=1 (FP8), E_out=2 (FP16, double-prec) :
            R       = 4096 / (C · E_in) = 4096 / 2 = 2048
            acc_max = R · E_out         = 2048 · 2 = 4096 B = 4 KB

        RFS_B size  = 4 KB

    → perfect match. The 4 KB constraint applies to (input tile,
       output tile, and worst-case acc) all by the same arithmetic.
       So RFS_B is SIZED to be the natural acc home.

  ─── The proposal : 4 linked architectural changes ─────────────────────

    1.  REDUCE-class instructions are UNARY :
           VREDUCE_SUM A_src               → result in RFS_B
           VROW_SUM_EXP A_src              → result in RFS_B
           VARGMAX A_src                   → result in RFS_B
           VTOPK_K_LE_4 A_src              → result in RFS_B
        Encoding : 1 src register only, dest is implicitly RFS_B.

    2.  RFS_B is REPURPOSED as accumulator during REDUCE :
        - reduce-phase : acc lives at RFS_B[0 .. R·E_out − 1]
        - broadcast-phase : entire RFS_B holds the broadcast output

    3.  Reduce-tree input WIDTH halves :  1024 B → 512 B
        - L0 cells : 512 → 256 (half)
        - depth (FP8 worst) : 10 → 9 (one level shorter)
        - cell library and per-cell architecture unchanged

    4.  Dedicated acc-FF bank from --accumulator-design Q1 is REMOVED.
        RFS_B's existing FFs absorb the role.
        ACC_SPILL latch stays (now sized to handle the read-before-
         write hazard of broadcast overwriting RFS_B).

  ─── Why the original conflicts dissolve ───────────────────────────────

    The earlier --accumulator-design Q1 argued for SEPARATE acc FFs
    based on three conflicts. With unary REDUCE, all three vanish :

    conflict (old)            new resolution
    ───────────────────────   ──────────────────────────────────────────
    Read-port conflict :      REDUCE never reads RFS_B as operand B.
      RFS_B can't serve       Only RMW acc port is active. NO conflict.
      binary-op B AND RMW
      acc same cycle

    3-src FMA conflict :      VFMA is BINARY class (reads A, B, C from
      C-operand from RFS_B    RFS_A, RFS_B, etc.). REDUCE is unary.
      vs RMW                  They are different instructions, never
                              dispatch in the same cycle to the same
                              ALU pipeline.

    Op-to-op pipelining :     op_n (REDUCE) writes broadcast result to
      op_n leaves acc in      RFS_B. op_{n+1} (BINARY) reads RFS_B as
      RFS_B → op_{n+1}        operand B = the broadcast result. This
      can't use RFS_B → bubble  is THE intended dataflow. No bubble.

    Resolution principle : separate INSTRUCTION CLASSES, not separate
    PHYSICAL RESOURCES. Cheaper and equally clean.

  ─── Resource and area accounting ──────────────────────────────────────

    resource             OLD design        NEW design        delta
                        (binary REDUCE)   (unary REDUCE)
    ─────────────────   ──────────────    ──────────────    ──────────
    Reduce tree         1024 B in,        512 B in,         −350 K gates
                        ~700 K gates      ~350 K gates
    Dedicated acc FF    2 KB / 50 K g     0 (in RFS_B)      −50 K gates
    ACC_SPILL latch     ~256 B / 7 K g    ~2 KB / 50 K g    +43 K gates
    RFS_B               4 KB (op B only)  4 KB (dual-role)  0
    RFS_A               4 KB              4 KB              0
    ──────────────────────────────────────────────────────  ──────────
    NET                                                     ~−350 K g

    ≈ 25 % of total VEC unit area. Plus :
       • simpler microcode (REDUCE has 1 src field, not 2)
       • simpler verification (REDUCE class has narrower input)
       • no port-arbitration logic between reduce-acc and op-B paths

  ─── ACC_SPILL latch sizing trick (reverse-order broadcast) ────────────

    Naïve broadcast (forward order) :
      beat 0 writes RFS_B[0..511] (row 0 broadcast)
      → overwrites acc[0..511/E_out-1] BEFORE later beats can read
        them. Must spill ALL acc first → ACC_SPILL = full acc size.

    REVERSE-order broadcast :
      beat 0 writes RFS_B[(R-1)*512..(R)*512]  (LAST row's broadcast)
      beat 1 writes RFS_B[(R-2)*512..(R-1)*512]
      ...
      acc lives at RFS_B[0 .. R·E_out − 1] (the START of RFS_B)
      → first overlap happens only at beat r* where
           (R-1-r*)·512 < R·E_out
        i.e. only the LAST few beats touch acc territory.

    Spill latch size needed = bytes of acc that the last few beats
    will overwrite ≈ R·E_out − (R-r_safe)·512 ≈ 64 B in practice.

    Worked example R=256, E_out=8 :
      acc region = [0 .. 2047]  (2 KB)
      first overlap at r* = 256 − ⌈2048/512⌉ − 1 = 251
      beats 0..251 are SAFE (write rows 4..255 of broadcast)
      beats 252..255 overwrite acc → spill needs 4·E_out = 32 B

    ⇒ ACC_SPILL latch shrinks from worst-case 2 KB to ~64 B.

  ─── Architecture diagram (revised) ────────────────────────────────────

                       RFS_A   (4 KB)
                          │
                          ▼ 512 B per beat
                  ┌──────────────────┐
                  │  REDUCE TREE      │  ← 512 B input, 9 levels
                  │  (unary, half      │     (down from 1024 B / 10 lvls)
                  │   the cells)       │
                  └────────┬──────────┘
                           │ K partials at chosen stop level
                           ▼
                  ┌──────────────────┐
                  │ K-WIDE RMW ALU    │◀───┐ same-cycle feedback
                  │ acc[r] += part[r] │    │
                  └────────┬──────────┘    │
                           │               │
                           ▼               │
                  ┌────────────────────┐   │
                  │  RFS_B   (4 KB)     │───┘
                  │                     │
                  │  during REDUCE :    │
                  │     [0 .. R·E_out)  │ ← acc
                  │     [rest]          │ ← don't-care
                  │                     │
                  │  during BROADCAST : │
                  │     full 4 KB       │ ← broadcast output
                  └────────┬───────────┘
                           │ broadcast phase only
                           ▼
                  ┌────────────────────┐
                  │  ACC_SPILL latch    │ ← small (~64 B w/ reverse
                  │  (read-before-write)│   broadcast trick)
                  └────────┬───────────┘
                           │
                           ▼
                  ┌────────────────────┐
                  │  BROADCAST TREE     │ ← also halves to 512 B output
                  └────────┬───────────┘
                           │
                           ▼
                       RFS_B (overwritten in REVERSE order)

  ─── Edge cases and how they are handled ───────────────────────────────

    case                              handling
    ───────────────────────────────   ──────────────────────────────────
    3-src FMA  (D = A·B + C)          BINARY class. RFS_B serves as
                                       C operand normally. Doesn't run
                                       concurrent with REDUCE → no issue.

    Dot product (Σ A_k · B_k)         2-instruction sequence :
                                         VFMUL  RFA_A ← A · B
                                         VREDUCE_SUM  acc ← RFA_A
                                       (per-lane FMA writes RFA_A,
                                        REDUCE then consumes it)

    REDUCE of a REDUCE result          Result of op_n is in RFS_B. To
                                       reduce it again, copy first :
                                         VMOV  RFS_A ← RFS_B
                                         VREDUCE  ...
                                       Rare pattern, 1-cycle copy is OK.

    GEMM per-element accumulation     Untouched. Per-lane FMA + RFA_A/B
                                       handle this completely outside
                                       the reduce-tree pipeline.

    Reduce + binary chain :           op_n (REDUCE) writes RFS_B with
      x = REDUCE(a)                    broadcast result. op_{n+1}
      y = op(x, c)                     (BINARY) reads RFS_B as B,
                                       gets x. Natural dataflow, no
                                       bubble.

  ─── Updated recommendation table ──────────────────────────────────────

    item                              OLD recommendation   NEW recommendation
    ───────────────────────────────   ──────────────────   ──────────────────
    REDUCE instruction class          binary (A and B)     UNARY (A only)
    Acc storage                       separate 2 KB FF     REUSE RFS_B
    Reduce-tree input width           1024 B (A | B)       512 B (A only)
    Operand-A vs B layout             PARALLEL `[A | B]`   N/A (B not used)
    L0 pair_mode bit                  needed for cross-AB  removed
    ACC_SPILL latch                   ~256 B               ~64 B with
                                                            reverse-broadcast
                                                            trick
    3-src FMA support                 RFS_B (dual-role)    RFS_B (B or C role)
    Dot product                       single-instr fused   2-instr sequence
                                       (binary REDUCE)      (FMUL → REDUCE)

  ─── Summary ───────────────────────────────────────────────────────────

    By moving complexity from MICROARCHITECTURE (separate physical
    acc FFs, 1024 B tree, per-cell pair_mode bits) to INSTRUCTION
    ARCHITECTURE (unary REDUCE class + clean dataflow ordering), we
    save ~350 K gates AND simplify microcode AND simplify verification.

    Cost paid : 1 extra instruction for fused dot-product (cheap),
                1 extra copy for reduce-of-reduce (rare).

    This is the recommended FINAL design.

══════════════════════════════════════════════════════════════════════════════
  PSEUDO-CODE  : row_sum_exp(in_spec, double_width=False)
══════════════════════════════════════════════════════════════════════════════

  Inputs:
    in_spec      : TileSpec(R, C, fmt_in)        # R · C · E_in = 4096
    double_width : bool                          # if True : E_out = 2·E_in
                                                 #          C_out = C / 2
  Output:
    out_spec     : TileSpec(R, C_out, fmt_out)   # R · C_out · E_out = 4096
    D[r, c]      = Σ_{c'=0..C-1} A[r, c']        # row sum cast to E_out,
                                                 # broadcast across all C_out cols.

  Hardware:
    RFS_A          : 4 KB,  holds input tile A.
    RFS_B          : 4 KB,  REPURPOSED.
                       phase 1 (reduce)    :
                         RFS_B[0 : R·E_out] holds R partial sums (the acc[]).
                         The remaining (4096 − R·E_out) bytes are don't-care.
                       phase 2 (broadcast) :
                         RFS_B is fully overwritten with the output tile D.
                       NOTE : original operand-B contents are DESTROYED.
    REDUCE_TREE    : 1024 B-input Benes-like front half (log2(N_in) ADD levels).
    BROADCAST_TREE : 1024 B-output Benes-like back half (log2(N_out) FANOUT levels).
    ACC_SPILL      : R · E_out byte latch. Reads RFS_B[0:R·E_out] in ONE cycle
                     between phase 1 and phase 2 — resolves the read-after-write
                     hazard between phase-1 acc[] and phase-2 D[].
                     ≤ 1 KB for all canonical shapes; physically a small SRAM
                     or DFF cluster sitting next to the broadcast tree input.

  Algorithm:

  ═══ PHASE 1 — REDUCE  (NUM_BEATS = 8 cycles : read entire RFS_A) ═══

      RFS_B[0 : R·E_out] ← 0                                  # zero acc[] slots

      for beat in 0 .. 7:
          A_beat ← RFS_A[beat·512 : beat·512 + 512]           # one tilelet

          if regime A (row_bytes_in ≤ 512):
              K = rows_per_input_beat                         # = 512 / row_bytes_in
              for k in 0 .. K-1:
                  row_id  = beat·K + k
                  elems   = A_beat's k-th row segment         # C elems × E_in
                  partial = REDUCE_TREE(elems, op=ADD,
                                        in_fmt=E_in, out_fmt=E_out,
                                        depth=ceil(log2(C)))
                  acc     = RFS_B[row_id·E_out : (row_id+1)·E_out]
                  RFS_B[row_id·E_out : (row_id+1)·E_out] ← acc + partial

          else (regime B, row_bytes_in > 512):
              row_id      = beat // beats_per_row_in
              beat_in_row = beat %  beats_per_row_in
              elems       = A_beat's elements                  # 512/E_in × E_in
              partial     = REDUCE_TREE(elems, op=ADD,
                                        in_fmt=E_in, out_fmt=E_out,
                                        depth=ceil(log2(512/E_in)))
              acc         = RFS_B[row_id·E_out : (row_id+1)·E_out]
              RFS_B[row_id·E_out : (row_id+1)·E_out] ← acc + partial
              # row_id is fully reduced when beat_in_row == beats_per_row_in - 1


  ═══ INTERLUDE — ACC_SPILL (1 cycle, hides hazard) ═══

      ACC_SPILL ← RFS_B[0 : R·E_out]                          # latch acc[]


  ═══ PHASE 2 — EXPAND  (NUM_BEATS = 8 cycles : write entire RFS_B) ═══

      for beat in 0 .. 7:
          D_beat = empty 512 B buffer

          if regime out A (row_bytes_out ≤ 512):
              K = rows_per_output_beat                        # = 512 / row_bytes_out
              for k in 0 .. K-1:
                  row_id = beat·K + k
                  sum_r  = ACC_SPILL[row_id]                  # 1 elem of E_out
                  copies = BROADCAST_TREE(sum_r, fanout=C_out,
                                          out_fmt=E_out)      # C_out × sum_r
                  D_beat's k-th row segment ← copies

          else (regime out B, row_bytes_out > 512):
              row_id      = beat // beats_per_row_out
              sum_r       = ACC_SPILL[row_id]
              copies      = BROADCAST_TREE(sum_r, fanout=512/E_out, out_fmt=E_out)
              D_beat      ← copies                           # 512/E_out copies

          RFS_B[beat·512 : beat·512 + 512] ← D_beat


  ═══ Total cycle count ═══
      Reduce      :  8 cy
      ACC_SPILL   :  1 cy   (single-cycle wide read + latch)
      Broadcast   :  8 cy
      ─────────────────
      Total       : 17 cy   (throughput = 1 ROW_SUM_EXP / 17 cy)

