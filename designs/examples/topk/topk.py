"""Top-K unified engine — single-file implementation.

This file replaces the previous ``stage_a.py`` + ``stage_b.py`` + ``local_sort.py``
+ ``merge_cell.py`` split. Everything lives here:

    - Section A: Pure Python schedule expansion (sort + half-merge tables).
    - Section B: Shared 128-cas bank + per-slot input mux + per-lane output demux.
    - Section C: Unified FSM walking the per-chunk phase sequence.
    - Section D: Running SRAM (K_MAX/P rows) + ``init_done`` + neg-inf fallback.
    - Section E: Direct 256-lane chunk_vals/chunk_idxs interface (tb-compatible).
    - Section F: Drain interface (one row × 256 lanes per cycle).
    - Section G: build() entry + CLI.

Per-chunk timing (P=256, rows_used = ceil(k_in / P)):

    fire (IDLE) -> SORT[0..L_S-1]      L_S = log2(P)*(log2(P)+1)/2 cy
                -> MERGE_PRE             1 cy   (load SRAM[0], reverse SORT result)
                -> for r in 0..rows_used-1:
                       MERGE_H[0..L_M-1]  L_M = 2*(log2(P)+1) cy
                       MERGE_POST          1 cy  (write SRAM[r], load next row)
                -> IDLE

For P=256: L_S=36, L_M=18. Total cy/chunk = 36 + 1 + (18+1)*rows_used.

Multi-fmt: 5 floating-point formats are selected at runtime via a 3-bit
``fmt_sel`` (bf16=0, fp16=1, fp32=2, fp8_e4m3=3, fp4_e2m1=4; 5..7 reserved).
"""
from __future__ import annotations

import math
from typing import List, Tuple

from pycircuit import (
    CycleAwareCircuit,
    CycleAwareDomain,
    compile_cycle_aware,
    module,
    u,
)
from pycircuit.hw import Reg, Wire, cat

from bitonic_schedule import (
    apply_schedule,
    gen_full_merge_2p_desc,
    gen_merge_half_schedule_2p,
    gen_sort_schedule_desc,
)
from cmp_swap import cmp_swap_const_dir
from fp_compare import fp_lt
from topk_config import (
    DEFAULT_PARAMS,
    FMTS_ORDERED,
    FMT_BF16, FMT_FP16, FMT_FP32, FMT_FP8_E4M3, FMT_FP4_E2M1,
    FMT_SEL_W,
    LARGE_PARAMS,
    NUM_FMTS,
    VAL_W,
    fmt_of,
    fmt_of_sel,
    k_in_w as k_in_w_of,
    rows_used_w as rows_used_w_of,
    validate_params,
)


_meta: dict = {}


# ═════════════════════════════════════════════════════════════════
# Section A — Pure Python schedule expansion
# ═════════════════════════════════════════════════════════════════

def build_sort_pair_table(P: int) -> List[List[Tuple[int, int, int]]]:
    """Return ``L_S`` layers of ``P/2`` ``(lo, hi, dir)`` pairs each."""
    sched = gen_sort_schedule_desc(P)
    return [list(pairs) for _stride, pairs in sched]


def build_merge_half_pair_table(P: int) -> List[List[Tuple[int, int, int]]]:
    """Return ``L_M`` half-layers of ``P/2`` ``(lo, hi, dir)`` pairs each.

    Each half-layer has exactly ``P/2`` pairs (the lo-half or hi-half of
    one full merge layer). The pairs cover lane indices in ``[0, 2P)``;
    half ``2k`` is the lo-half of full layer ``k`` (lanes < P), half
    ``2k+1`` is the hi-half (lanes >= P).
    """
    sched = gen_merge_half_schedule_2p(P)
    return [list(pairs) for _stride, pairs in sched]


# ═════════════════════════════════════════════════════════════════
# Section H — Pure Python software model of the engine (selftest hook)
# ═════════════════════════════════════════════════════════════════

def simulate_engine_python(
    chunks_pairs: list[list[tuple[int, int]]],
    *,
    K: int,
    P: int,
    K_MAX: int,
    fmt,
) -> list[list[tuple[int, int]]]:
    """Python software model that mirrors the unified engine cycle-by-cycle.

    Returns the running SRAM state after all chunks have been absorbed: a
    list of ``K_MAX/P`` rows, each ``P`` ``(val_bits, idx)`` pairs in
    descending order. Rows beyond ``rows_used`` may contain stale or
    init-default values; the caller should slice ``[:rows_used]`` to get
    the final Top-K.
    """
    from tool import fp_to_unsigned_key

    n_rows_max = K_MAX // P
    rows_used = (K + P - 1) // P
    assert 1 <= rows_used <= n_rows_max
    sort_table  = build_sort_pair_table(P)
    half_table  = build_merge_half_pair_table(P)
    init_done = [False] * n_rows_max
    neg_inf_pair = (fmt.neg_inf_bits, 0)
    rows: list[list[tuple[int, int]]] = [[neg_inf_pair] * P for _ in range(n_rows_max)]

    def cmp_swap_pair(a, b, direction):
        ka = fp_to_unsigned_key(a[0], fmt)
        kb = fp_to_unsigned_key(b[0], fmt)
        lt = ka < kb
        swap = lt if direction == 1 else not lt
        return (b, a) if swap else (a, b)

    def apply_layer_pairs(arr, pairs):
        out = list(arr)
        for lo, hi, direction in pairs:
            out[lo], out[hi] = cmp_swap_pair(out[lo], out[hi], direction)
        return out

    for ch in chunks_pairs:
        assert len(ch) == P
        # SORT phase: 36 layers walk lane_regs[0..P-1]
        sort_lanes = list(ch)
        for layer_pairs in sort_table:
            sort_lanes = apply_layer_pairs(sort_lanes, layer_pairs)
        # SORT result is descending. Reverse into carry slot for valley-bitonic.
        carry = list(sort_lanes)
        for r in range(rows_used):
            # MERGE_PRE: load SRAM[r] (with init_done mask) into A side.
            row_a = list(rows[r]) if init_done[r] else [neg_inf_pair] * P
            # Build merge lane array: A ++ reversed(B) (carry).
            lanes = list(row_a) + list(reversed(carry))
            # MERGE_H: 18 half-layers
            for half_pairs in half_table:
                lanes = apply_layer_pairs(lanes, half_pairs)
            # Top P -> SRAM[r]; bot P -> next carry
            rows[r] = lanes[:P]
            init_done[r] = True
            carry = lanes[P:]
    return rows


def simulate_engine_topk_keys(
    chunks_pairs: list[list[tuple[int, int]]],
    *,
    K: int,
    P: int,
    K_MAX: int,
    fmt,
) -> list[tuple[int, int]]:
    """Convenience: top-K (val_bits, idx) pairs descending across rows."""
    rows = simulate_engine_python(
        chunks_pairs, K=K, P=P, K_MAX=K_MAX, fmt=fmt,
    )
    rows_used = (K + P - 1) // P
    flat = [pair for row in rows[:rows_used] for pair in row]
    return flat[:K]


# ═════════════════════════════════════════════════════════════════
# Section B helpers — bus packing
# ═════════════════════════════════════════════════════════════════

def _unpack_lanes(bus: Wire, *, lane_w: int, lanes: int) -> List[Wire]:
    return [bus.slice(lsb=i * lane_w, width=lane_w) for i in range(lanes)]


def _pack_lanes_lsb_first(lanes: List[Wire]) -> Wire:
    return cat(*reversed(lanes))


# ═════════════════════════════════════════════════════════════════
# Section B helpers — phase mux tree (small fan-in is fine; clarity matters)
# ═════════════════════════════════════════════════════════════════

def _mux_phase(m, sel: Wire, options: List[Wire], *, default: Wire) -> Wire:
    """Linear mux ``options[sel]`` where sel runs 0..len(options)-1.

    Out-of-range sel returns ``default``. Implemented as a chain of
    select() so that each phase index has a clear path through the netlist.
    The chain length equals ``len(options)`` (≤ 54 for P=256), keeping the
    structure flat and synthesizable.
    """
    cur: Wire = default
    sel_w = sel.width
    for i, opt in enumerate(options):
        eq = sel == m.const(i, width=sel_w)
        cur = eq.select(opt, cur)
    return cur


# ── @module entry: thin wrapper that delegates to _build_topk_engine ──
@module
def build(
    m: CycleAwareCircuit,
    domain: CycleAwareDomain,
    *,
    P: int = DEFAULT_PARAMS["P"],
    K_MAX: int = DEFAULT_PARAMS["K_MAX"],
    idx_w: int = DEFAULT_PARAMS["idx_w"],
) -> None:
    """Top-K unified engine entry.

    Compile-time parameters
    -----------------------
    - ``P``     : sort/merge unit size (chunk width). Power of 2, >= 2.
    - ``K_MAX`` : max K supported. Power of 2, multiple of P. SRAM has
                  K_MAX/P rows.
    - ``idx_w`` : index width (bits). 13 covers fp4 worst-case N=8192/tile.

    Runtime ports
    -------------
    Inputs:
        - ``chunk_vals``      : P*VAL_W packed bus, lane 0 in LSB.
        - ``chunk_idxs``      : P*idx_w packed bus.
        - ``valid_in``        : 1-bit handshake (sampled with ready_out).
        - ``fmt_sel``         : 3-bit fmt selector.
        - ``k_in``            : runtime K, k_in_w(K_MAX) bits.
        - ``topk_drain_addr`` : log2(K_MAX/P) bits; selects which SRAM row
                                to expose via ``topk_vals/topk_idxs``.

    Outputs:
        - ``topk_vals``    : P*VAL_W packed bus.
        - ``topk_idxs``    : P*idx_w packed bus.
        - ``running_valid``: sticky bit set after first chunk fully absorbed.
        - ``ready_out``    : 1 in IDLE, 0 while engine is processing.
    
    Build the unified Top-K engine inline. Called from ``build()``.

    Lives outside ``@module`` so the JIT trace falls through to Python for
    the heavy schedule pre-processing (list comprehensions, dict-style
    inverse mappings, etc.) and only the actual ``m.input/m.out/m.assign``
    calls inside this body are observed as hardware-carrying ops.
    """
    validate_params(P=P, K_MAX=K_MAX, idx_w=idx_w)

    cd = domain.clock_domain
    log2_P = int(math.log2(P))
    L_S = log2_P * (log2_P + 1) // 2       # sort layers, e.g. P=256 -> 36
    L_M = 2 * (log2_P + 1)                 # merge half-layers, e.g. P=256 -> 18
    half_P = P // 2                        # cas slots per phase, e.g. 128 for P=256
    n_rows_max = K_MAX // P
    addr_w = max(1, (n_rows_max - 1).bit_length())
    k_in_width = k_in_w_of(K_MAX)
    rows_used_width = rows_used_w_of(P, K_MAX)

    # Phase encoding:
    #   big_phase: 2 bits — IDLE=0, SORT=1, MERGE_PRE=2, MERGE_HP=3
    #   ph_step  : substep within big_phase
    PH_IDLE      = 0
    PH_SORT      = 1
    PH_MERGE_PRE = 2
    PH_MERGE_HP  = 3
    big_w = 2
    # ph_step max value across phases: max(L_S, L_M+1) -> +1 for MERGE_POST as substep=L_M
    step_max = max(L_S - 1, L_M)  # SORT: 0..L_S-1; MERGE_HP: 0..L_M (last == POST)
    step_w = max(1, step_max.bit_length())

    # ── Build pair tables (Python pre-compute) ──
    sort_pairs  = build_sort_pair_table(P)         # [L_S][P/2] of (lo, hi, dir)
    merge_pairs = build_merge_half_pair_table(P)   # [L_M][P/2] of (lo, hi, dir)
    assert len(sort_pairs) == L_S and len(merge_pairs) == L_M
    for lp in sort_pairs:  assert len(lp) == half_P
    for lp in merge_pairs: assert len(lp) == half_P

    # Per-lane inverse mapping: which (slot k, port lo|hi) covers lane j
    # in each phase? None = lane not touched in that phase.
    sort_lane_src: List[List[Tuple[int, str] | None]] = [
        [None] * L_S for _ in range(P)
    ]
    for li, pairs in enumerate(sort_pairs):
        for k, (lo, hi, _d) in enumerate(pairs):
            sort_lane_src[lo][li] = (k, "lo")
            sort_lane_src[hi][li] = (k, "hi")
    for j in range(P):
        for li in range(L_S):
            assert sort_lane_src[j][li] is not None, (
                f"sort lane {j} not covered by layer {li}"
            )

    merge_lane_src: List[List[Tuple[int, str] | None]] = [
        [None] * L_M for _ in range(2 * P)
    ]
    for li, pairs in enumerate(merge_pairs):
        for k, (lo, hi, _d) in enumerate(pairs):
            merge_lane_src[lo][li] = (k, "lo")
            merge_lane_src[hi][li] = (k, "hi")
    # Half ``2k`` (lo-half) covers lanes < P; half ``2k+1`` (hi-half) covers lanes >= P.
    # So for a given lane in [0, 2P), only half of the L_M half-layers cover it.
    # Rest stay as None and the lane-reg holds in those substeps.

    # ── I/O ports ──
    chunk_vals = m.input("chunk_vals", width=P * VAL_W)
    chunk_idxs = m.input("chunk_idxs", width=P * idx_w)
    valid_in   = m.input("valid_in",   width=1)
    fmt_sel    = m.input("fmt_sel",    width=FMT_SEL_W)
    k_in       = m.input("k_in",       width=k_in_width)
    drain_addr = m.input("topk_drain_addr", width=addr_w)

    chunk_v = _unpack_lanes(chunk_vals, lane_w=VAL_W,  lanes=P)
    chunk_i = _unpack_lanes(chunk_idxs, lane_w=idx_w,  lanes=P)

    # ── Compute rows_used (runtime) = ceil(k_in / P) ──
    one_kw    = m.const(1, width=k_in_width)
    k_minus_1 = (k_in.as_unsigned() - one_kw)
    if log2_P >= k_in_width:
        rows_used_m1 = m.const(0, width=rows_used_width)
    else:
        rows_used_m1_full = k_minus_1.slice(lsb=log2_P, width=k_in_width - log2_P)
        rows_used_m1 = rows_used_m1_full.slice(lsb=0, width=rows_used_width)
    last_row_target = rows_used_m1.slice(lsb=0, width=addr_w)

    # ── State regs ──
    big_phase = m.out("topk_big_phase", domain=cd, width=big_w,  init=u(big_w, PH_IDLE))
    ph_step   = m.out("topk_ph_step",   domain=cd, width=step_w, init=u(step_w, 0))
    row_cnt   = m.out("topk_row_cnt",   domain=cd, width=addr_w, init=u(addr_w, 0))
    init_done = m.out("topk_init_done", domain=cd, width=n_rows_max, init=u(n_rows_max, 0))
    vseen     = m.out("topk_vseen",     domain=cd, width=1, init=u(1, 0))
    raddr_d   = m.out("topk_raddr_d",   domain=cd, width=addr_w, init=u(addr_w, 0))

    in_idle  = big_phase.out() == m.const(PH_IDLE, width=big_w)
    in_sort  = big_phase.out() == m.const(PH_SORT, width=big_w)
    in_pre   = big_phase.out() == m.const(PH_MERGE_PRE, width=big_w)
    in_hp    = big_phase.out() == m.const(PH_MERGE_HP, width=big_w)
    ready    = in_idle
    fire     = valid_in & ready

    # In MERGE_HP, ph_step==L_M means MERGE_POST; otherwise it's a half-layer index.
    last_post_step = m.const(L_M, width=step_w)
    is_post = in_hp & (ph_step.out() == last_post_step)
    last_sort_step = m.const(L_S - 1, width=step_w)
    is_sort_last = in_sort & (ph_step.out() == last_sort_step)
    last_hp_step = m.const(L_M - 1, width=step_w)
    is_hp_last_layer = in_hp & (ph_step.out() == last_hp_step)
    last_row = row_cnt.out() == last_row_target
    finishing = is_post & last_row

    # ── Lane registers: 2*P unified lanes ──
    #   lane_v[0..P-1]  : SORT chunk + MERGE row (A)
    #   lane_v[P..2P-1] : MERGE carry (B), held in valley-bitonic order
    lane_vr: List[Reg] = [
        m.out(f"topk_lane_v{j}", domain=cd, width=VAL_W,  init=u(VAL_W, 0))
        for j in range(2 * P)
    ]
    lane_ir: List[Reg] = [
        m.out(f"topk_lane_i{j}", domain=cd, width=idx_w,  init=u(idx_w, 0))
        for j in range(2 * P)
    ]
    lane_v_now = [r.out() for r in lane_vr]
    lane_i_now = [r.out() for r in lane_ir]

    # ── Section D: SRAM + raddr prefetch + neg-inf row ──
    lane_pack_w = VAL_W + idx_w
    row_bus_w = P * lane_pack_w
    wstrb_w = (row_bus_w + 7) // 8
    wstrb_const = m.const((1 << wstrb_w) - 1, width=wstrb_w)

    # Forward-decl wdata wire (resolved after we know the merge result lanes).
    wdata_wire = m.new_wire(width=row_bus_w)
    wdata_zero = m.const(0, width=row_bus_w)
    sram_wvalid = is_post                       # write commits on the cycle of POST
    wdata_eff = sram_wvalid.select(wdata_wire, wdata_zero)

    # raddr selects the row whose data we want on sram_rdata next cycle.
    # During SORT_LAST cycle: prefetch row 0 (so MERGE_PRE can read it).
    # During HP_LAST_LAYER cycle: prefetch row_cnt+1 (so next-row MERGE_PRE_INTER reads it).
    # Otherwise: drive drain_addr (steady drain in IDLE).
    next_row_for_merge = (row_cnt.out() + m.const(1, width=addr_w)).slice(
        lsb=0, width=addr_w
    )
    raddr_sort_last = m.const(0, width=addr_w)
    # Default raddr: drain_addr (so IDLE drives the drain row to the read port).
    # SORT[L_S-1]: drive 0; HP[L_M-1]: drive row_cnt+1; POST: drive drain_addr.
    raddr_choice_a = is_hp_last_layer.select(next_row_for_merge, drain_addr)
    raddr = is_sort_last.select(raddr_sort_last, raddr_choice_a)

    sram_rdata = m.sync_mem(
        cd.clk, cd.rst,
        ren=m.const(1, width=1),
        raddr=raddr,
        wvalid=sram_wvalid,
        waddr=row_cnt.out(),
        wdata=wdata_eff,
        wstrb=wstrb_const,
        depth=n_rows_max,
        name="topk_sram",
    )

    # raddr_d tracks raddr by 1 cycle so init_done lookup matches sram_rdata's row.
    raddr_d.set(raddr)
    init_done_bus = init_done.out()
    init_done_bit = init_done_bus.lshr(amount=raddr_d.out()).slice(lsb=0, width=1)

    # Build neg-inf row to substitute when init_done[r] == 0.
    neg_inf_consts = [m.const(f.neg_inf_bits, width=VAL_W) for f in FMTS_ORDERED]
    neg_inf_val = _mux_phase(m, fmt_sel, neg_inf_consts, default=neg_inf_consts[FMT_FP32])
    zero_idx    = m.const(0, width=idx_w)
    sram_lanes = _unpack_lanes(sram_rdata, lane_w=VAL_W + idx_w, lanes=P)
    sram_row_v_raw = [ln.slice(lsb=0, width=VAL_W) for ln in sram_lanes]
    sram_row_i_raw = [ln.slice(lsb=VAL_W, width=idx_w) for ln in sram_lanes]
    sram_row_v = [init_done_bit.select(sram_row_v_raw[i], neg_inf_val) for i in range(P)]
    sram_row_i = [init_done_bit.select(sram_row_i_raw[i], zero_idx)    for i in range(P)]

    # ── Section B: shared 128-cas bank ──
    # Per cas slot k: build per-port mux trees over (L_S sort layers + L_M merge half-layers).
    slot_v_lo: List[Wire] = [None] * half_P  # type: ignore[list-item]
    slot_i_lo: List[Wire] = [None] * half_P  # type: ignore[list-item]
    slot_v_hi: List[Wire] = [None] * half_P  # type: ignore[list-item]
    slot_i_hi: List[Wire] = [None] * half_P  # type: ignore[list-item]

    # Combined phase index for the cas bank: 0..L_S-1 = sort layers; L_S..L_S+L_M-1 = merge half-layers.
    total_phases = L_S + L_M
    phase_idx_w = max(1, (total_phases - 1).bit_length())
    # Compute the combined phase index from (big_phase, ph_step):
    #   if in_sort:    phase_idx = ph_step                    [0..L_S-1]
    #   if in_hp:      phase_idx = L_S + ph_step              [L_S..L_S+L_M-1]
    #   else (idle, pre, post): phase_idx = 0 (cas output unused — `when` gates)
    ph_step_widened = ph_step.out().slice(lsb=0, width=phase_idx_w) if step_w >= phase_idx_w \
                      else cat(m.const(0, width=phase_idx_w - step_w), ph_step.out())
    hp_phase_idx = (m.const(L_S, width=phase_idx_w).as_unsigned()
                    + ph_step_widened.as_unsigned()).slice(lsb=0, width=phase_idx_w)
    phase_idx = in_hp.select(hp_phase_idx, ph_step_widened)

    for k in range(half_P):
        v_a_opts: List[Wire] = []
        v_b_opts: List[Wire] = []
        i_a_opts: List[Wire] = []
        i_b_opts: List[Wire] = []
        dirs: List[int] = []
        # Sort layers first.
        for layer_pairs in sort_pairs:
            lo, hi, direction = layer_pairs[k]
            v_a_opts.append(lane_v_now[lo])
            v_b_opts.append(lane_v_now[hi])
            i_a_opts.append(lane_i_now[lo])
            i_b_opts.append(lane_i_now[hi])
            dirs.append(direction)
        # Merge half-layers.
        for half_pairs in merge_pairs:
            lo, hi, direction = half_pairs[k]
            v_a_opts.append(lane_v_now[lo])
            v_b_opts.append(lane_v_now[hi])
            i_a_opts.append(lane_i_now[lo])
            i_b_opts.append(lane_i_now[hi])
            dirs.append(direction)

        v_a = _mux_phase(m, phase_idx, v_a_opts, default=v_a_opts[0])
        v_b = _mux_phase(m, phase_idx, v_b_opts, default=v_b_opts[0])
        i_a = _mux_phase(m, phase_idx, i_a_opts, default=i_a_opts[0])
        i_b = _mux_phase(m, phase_idx, i_b_opts, default=i_b_opts[0])

        # Run two cas (one DESC, one ASC) and pick by phase. Sort layers can
        # have either direction; merge half-layers are always DESC.
        v_lo_d, i_lo_d, v_hi_d, i_hi_d, _ = cmp_swap_const_dir(
            v_a, i_a, v_b, i_b, direction=1, fmt_sel=fmt_sel,
        )
        # ASC variant: only needed if any sort layer at this slot is ASC.
        any_asc = any(d == 0 for d in dirs)
        if any_asc:
            v_lo_a, i_lo_a, v_hi_a, i_hi_a, _ = cmp_swap_const_dir(
                v_a, i_a, v_b, i_b, direction=0, fmt_sel=fmt_sel,
            )
            # Build a 1-bit dir-select from phase_idx via a mux of constants.
            dir_opts = [m.const(d, width=1) for d in dirs]
            sel_dir_w = _mux_phase(m, phase_idx, dir_opts, default=dir_opts[0])
            v_lo = sel_dir_w.select(v_lo_d, v_lo_a)
            v_hi = sel_dir_w.select(v_hi_d, v_hi_a)
            i_lo = sel_dir_w.select(i_lo_d, i_lo_a)
            i_hi = sel_dir_w.select(i_hi_d, i_hi_a)
        else:
            v_lo, i_lo, v_hi, i_hi = v_lo_d, i_lo_d, v_hi_d, i_hi_d

        slot_v_lo[k] = v_lo
        slot_i_lo[k] = i_lo
        slot_v_hi[k] = v_hi
        slot_i_hi[k] = i_hi

    # ── Section B continued: per-lane next-state from cas outputs ──
    # For each lane j, build the per-phase cas-output options (or None if not covered).
    # Then mux them together with per-phase sources for non-cas phases (chunk
    # load on fire, SRAM load on MERGE_PRE / MERGE_POST→next-row, carry
    # reversal during the same transitions).
    cas_v_for_lane = [list[Wire | None]() for _ in range(2 * P)]
    cas_i_for_lane = [list[Wire | None]() for _ in range(2 * P)]
    for j in range(2 * P):
        cas_v_for_lane[j] = [None] * total_phases
        cas_i_for_lane[j] = [None] * total_phases
    # Sort phase cas outputs cover lanes 0..P-1.
    for li in range(L_S):
        for k, (lo, hi, _d) in enumerate(sort_pairs[li]):
            cas_v_for_lane[lo][li] = slot_v_lo[k]
            cas_i_for_lane[lo][li] = slot_i_lo[k]
            cas_v_for_lane[hi][li] = slot_v_hi[k]
            cas_i_for_lane[hi][li] = slot_i_hi[k]
    # Merge half-layer cas outputs cover lanes 0..2P-1.
    for li in range(L_M):
        ph = L_S + li
        for k, (lo, hi, _d) in enumerate(merge_pairs[li]):
            cas_v_for_lane[lo][ph] = slot_v_lo[k]
            cas_i_for_lane[lo][ph] = slot_i_lo[k]
            cas_v_for_lane[hi][ph] = slot_v_hi[k]
            cas_i_for_lane[hi][ph] = slot_i_hi[k]

    # ── Lane next-value selection ──
    for j in range(2 * P):
        # Cas-output mux across phases (default = hold current value).
        v_opts: List[Wire] = []
        i_opts: List[Wire] = []
        for p in range(total_phases):
            cas_v_p = cas_v_for_lane[j][p]
            cas_i_p = cas_i_for_lane[j][p]
            v_opts.append(cas_v_p if cas_v_p is not None else lane_v_now[j])
            i_opts.append(cas_i_p if cas_i_p is not None else lane_i_now[j])
        v_from_cas = _mux_phase(m, phase_idx, v_opts, default=lane_v_now[j])
        i_from_cas = _mux_phase(m, phase_idx, i_opts, default=lane_i_now[j])

        # Non-cas sources: priority handling per phase combination.
        if j < P:
            # Lower lanes: chunk load on fire; SRAM row load during MERGE_PRE / MERGE_POST→next-row.
            v_from_chunk = chunk_v[j]
            i_from_chunk = chunk_i[j]
            v_from_sram  = sram_row_v[j]
            i_from_sram  = sram_row_i[j]
            # Pick: fire → chunk; in_pre → sram; in_post & ~last_row → sram; else → cas
            sram_load = in_pre | (is_post & ~last_row)
            v_next = fire.select(
                v_from_chunk,
                sram_load.select(v_from_sram, v_from_cas),
            )
            i_next = fire.select(
                i_from_chunk,
                sram_load.select(i_from_sram, i_from_cas),
            )
            # Update enable: fire OR engine running (sort/hp) OR transition cycles.
            update_en = fire | in_sort | in_hp | in_pre
        else:
            # Upper lanes (carry slot): reversed load during MERGE_PRE (from lane_v_now[P-1-(j-P)])
            # and during MERGE_POST→next-row (from lane_v_now[2P-1-(j-P) + P] = lane_v_now[3P-1-j+P]
            # — wait, just lane[P + (P-1 - (j - P))] = lane[2P-1-(j-P)] for both).
            # Mathematically: reverse the carry slot in place (lane[P+t] ← lane[P+(P-1-t)]) on POST,
            # and reverse the SORT result (lane[P+t] ← lane[P-1-t]) on PRE.
            t = j - P  # 0..P-1
            v_pre_src  = lane_v_now[P - 1 - t]              # SORT result reversed
            i_pre_src  = lane_i_now[P - 1 - t]
            v_post_src = lane_v_now[P + (P - 1 - t)]        # bot-P reversed
            i_post_src = lane_i_now[P + (P - 1 - t)]
            v_next = in_pre.select(
                v_pre_src,
                (is_post & ~last_row).select(v_post_src, v_from_cas),
            )
            i_next = in_pre.select(
                i_pre_src,
                (is_post & ~last_row).select(i_post_src, i_from_cas),
            )
            update_en = in_hp | in_pre | (is_post & ~last_row)

        lane_vr[j].set(v_next, when=update_en)
        lane_ir[j].set(i_next, when=update_en)

    # ── Resolve forward-decl wdata: top-P merge result packed ──
    # During POST cycle, the lane_regs hold the merge output (committed at the
    # previous cycle's posedge from MERGE_HP last layer). So wdata = packed
    # current lane_regs[0..P-1] with lane[i] = (idx<<VAL_W) | val, LSB-first.
    wdata_lane_packs = [cat(lane_i_now[i], lane_v_now[i]) for i in range(P)]
    m.assign(wdata_wire, _pack_lanes_lsb_first(wdata_lane_packs))

    # ── FSM next-state ──
    sort_step_inc = (ph_step.out().as_unsigned()
                     + m.const(1, width=step_w).as_unsigned()).slice(lsb=0, width=step_w)
    hp_step_inc = sort_step_inc

    # big_phase next:
    #   IDLE + fire    -> SORT
    #   SORT, last     -> MERGE_PRE
    #   SORT, !last    -> SORT
    #   MERGE_PRE      -> MERGE_HP
    #   MERGE_HP, !post-> MERGE_HP
    #   POST + last_row-> IDLE
    #   POST + ~last   -> MERGE_HP
    bp_idle = m.const(PH_IDLE,      width=big_w)
    bp_sort = m.const(PH_SORT,      width=big_w)
    bp_pre  = m.const(PH_MERGE_PRE, width=big_w)
    bp_hp   = m.const(PH_MERGE_HP,  width=big_w)
    next_big = (
        in_idle.select(
            fire.select(bp_sort, bp_idle),
            in_sort.select(
                is_sort_last.select(bp_pre, bp_sort),
                in_pre.select(
                    bp_hp,
                    # in_hp:
                    is_post.select(
                        finishing.select(bp_idle, bp_hp),
                        bp_hp,
                    ),
                ),
            ),
        )
    )
    big_phase.set(next_big)

    # ph_step next: depends on big_phase transitions
    zero_step = m.const(0, width=step_w)
    next_ph_step_in_sort = is_sort_last.select(zero_step, sort_step_inc)
    next_ph_step_in_hp = (
        is_post.select(
            zero_step,                          # POST → reset for next row's MERGE_H[0] (or IDLE)
            hp_step_inc,                        # otherwise advance in MERGE_H
        )
    )
    # Default ph_step: hold (overridden per-state below).
    next_ph_step = (
        in_idle.select(
            fire.select(zero_step, ph_step.out()),
            in_sort.select(
                next_ph_step_in_sort,
                in_pre.select(
                    zero_step,                  # MERGE_PRE → MERGE_HP[0]
                    next_ph_step_in_hp,
                ),
            ),
        )
    )
    ph_step.set(next_ph_step)

    # row_cnt next: 0 on fire (SORT start) and on POST when finishing; +1 on POST otherwise.
    zero_addr = m.const(0, width=addr_w)
    inc_row = (row_cnt.out().as_unsigned()
               + m.const(1, width=addr_w).as_unsigned()).slice(lsb=0, width=addr_w)
    next_row = (
        in_idle.select(
            fire.select(zero_addr, row_cnt.out()),
            in_sort.select(row_cnt.out(),
                in_pre.select(zero_addr,
                    is_post.select(
                        finishing.select(zero_addr, inc_row),
                        row_cnt.out(),
                    ),
                ),
            ),
        )
    )
    row_cnt.set(next_row)

    # init_done update: set bit row_cnt at POST (committed write).
    one_const_n = m.const(1, width=n_rows_max)
    one_hot_at_row = one_const_n.shl(amount=row_cnt.out()).slice(lsb=0, width=n_rows_max)
    new_init_done = init_done_bus | one_hot_at_row
    init_done.set(is_post.select(new_init_done, init_done_bus))

    # vseen: latch once any chunk's POST has fully completed.
    vseen.set(vseen.out() | finishing)

    # ── Drain output: row at drain_addr from SRAM ──
    # sram_row_v/_i already include the init_done mask, but they reflect the
    # row at raddr_d (i.e. the previous cycle's read). When the engine is in
    # IDLE, raddr is driven from drain_addr → sram_rdata == mem[drain_addr]
    # combinationally one cycle later. The drain consumer should hold
    # drain_addr stable for at least 2 cycles to read a row.
    out_v_bus = _pack_lanes_lsb_first(sram_row_v)
    out_i_bus = _pack_lanes_lsb_first(sram_row_i)
    m.output("topk_vals", out_v_bus)
    m.output("topk_idxs", out_i_bus)
    m.output("running_valid", vseen.out())
    m.output("ready_out", ready)

    # ── Meta info for CLI ──
    _meta.update({
        "P": P, "K_MAX": K_MAX, "idx_w": idx_w,
        "n_rows_max": n_rows_max,
        "k_in_w": k_in_width,
        "L_S": L_S, "L_M": L_M,
        "cas_per_bank": half_P,
        "cy_per_chunk_min": L_S + 1 + (L_M + 1),               # rows_used=1
        "cy_per_chunk_max": L_S + 1 + (L_M + 1) * n_rows_max,  # rows_used=K_MAX/P
    })



build.__pycircuit_name__ = "topk"


# ═════════════════════════════════════════════════════════════════
# CLI: build the unified module and a few smaller variants
# ═════════════════════════════════════════════════════════════════

if __name__ == "__main__":
    print("Building topk unified MLIR (single shared 128-cas bank, 5 fmts) ...")
    cases = [
        # (P, K_MAX, idx_w)
        (4,    16,   4),     # smoke
        (16,   64,   8),
        (32,  256,  10),
        (64,  256,  12),
        (256, 4096, 16),     # primary target
    ]
    for (P, K_MAX, idx_w) in cases:
        _meta.clear()
        circuit = compile_cycle_aware(
            build,
            name=f"topk_uni_P{P}_KM{K_MAX}",
            P=P, K_MAX=K_MAX, idx_w=idx_w,
        )
        mlir = circuit.emit_mlir()
        print(f"  P={P:3d} K_MAX={K_MAX:5d} idx_w={idx_w:2d}:"
              f"  L_S={_meta['L_S']:>3d}  L_M={_meta['L_M']:>3d}"
              f"  cas_bank={_meta['cas_per_bank']:>3d}"
              f"  cy/chunk={_meta['cy_per_chunk_min']:>3d}..{_meta['cy_per_chunk_max']:>4d}"
              f"  k_in_w={_meta['k_in_w']:>2d}"
              f"  MLIR={len(mlir):>10d} chars")
    print("All topk unified builds passed.")

