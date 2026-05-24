"""Top-K Histogram accelerator — top-level pycircuit `build` module.

Implements the radix-select Top-K pipeline from
[designs/topk-histogram/arch.md](designs/topk-histogram/arch.md).

Design choices (v1):
  - 256-bin histogram engine, 128-lane popcount per bin per cycle
  - Pipelined cumsum: ``CUMSUM_BINS_PER_CY`` bins per PH_CUMSUM cycle (default 16)
  - Pipelined filter prefix: ``FILTER_LANES_PER_CY`` lanes per PH_FILTER cycle (default 16)
  - 1024-bit mask reg, updated by a separate 8-cy MASK_UPD pass per round
  - Filter writes into a flat 1024-lane output_buf (val + idx, 32-bit each)
    via prefix-sum compact + per-bank reg select (barrel align inline in build)

State allocations (m.out / m.sync_mem) are all inline because plain helpers
trigger the JIT structural-metrics check on `state_call_count > 0`.
Combinational helpers (no state) live in `datapath.py` / `histogram_engine.py` /
`filter_output.py`.

Removed-API note: inside `@module build`, use ternary `a if c else b` (PYC430).
Helpers may still use legacy Wire APIs; the JIT dispatches to `fn(*args)` and
never scans helper ASTs.
"""
from __future__ import annotations

from pycircuit import (
    CycleAwareCircuit,
    CycleAwareDomain,
    compile_cycle_aware,
    module,
    u,
)
from pycircuit.hw import cat

from topk_histogram_config import DEFAULT_PARAMS, validate_params
from fp_key import fp32_to_sortable_key_hw, sortable_key_to_fp32_hw
from datapath import (
    kth_compose,
    mux_phase,
    pack_lanes_lsb_first,
    unpack_lanes,
)
from histogram_engine import (
    cumsum_chunk_step,
    cumsum_num_chunks,
    hist_cycle_counts,
    mask_refine_lane_bits,
    mask_row_replace,
    mask_row_lanes_for_beat,
    prior_round_idx,
    target_bin_for_mask,
)
from filter_output import (
    filter_chunk_lanes,
    filter_num_chunks,
    filter_pass_lanes,
    popcount_bits,
    prefix_sum_chunk,
)


_meta: dict = {}


# Phase encoding (main_phase)
#
# Single-pass FILTER: GT and EQ predicates are evaluated together each cycle.
# eq_keep is precomputed at the end of CUMSUM round 3 (last chunk) from
# cum_hist_at_target + bottomK (see §12 below), so we don't need a separate FILT_GT pass to learn gt_count.
PH_IDLE     = 0
PH_LOAD     = 1
PH_HIST     = 2
PH_CUMSUM   = 3
PH_MASK     = 4
PH_FILTER   = 5
PH_WAIT_OUT = 6
PH_DRAIN    = 7
PH_W = 3

# sub_step width (covers 0..7 for 8-cycle phases)
SUBSTEP_W = 4

NUM_BINS = DEFAULT_PARAMS["NUM_BINS"]
RADIX_BITS = DEFAULT_PARAMS["RADIX_BITS"]


@module
def build(
    m: CycleAwareCircuit,
    domain: CycleAwareDomain,
    *,
    N: int = DEFAULT_PARAMS["N"],
    LANE_NUM: int = DEFAULT_PARAMS["LANE_NUM"],
    BURST_LEN: int = DEFAULT_PARAMS["BURST_LEN"],
    K_MAX: int = DEFAULT_PARAMS["K_MAX"],
    K_MAX_BITS: int = DEFAULT_PARAMS["K_MAX_BITS"],
    HIST_W: int = DEFAULT_PARAMS["HIST_W"],
    VAL_W: int = DEFAULT_PARAMS["VAL_W"],
    ELEM_IDX_W: int = DEFAULT_PARAMS["ELEM_IDX_W"],
    NUM_BINS: int = DEFAULT_PARAMS["NUM_BINS"],
    CUMSUM_BINS_PER_CY: int = DEFAULT_PARAMS["CUMSUM_BINS_PER_CY"],
    FILTER_LANES_PER_CY: int = DEFAULT_PARAMS["FILTER_LANES_PER_CY"],
) -> None:
    """arch.md §3.1 contract: 1024 fp32 in 8-beat burst → top-K out 8-beat burst."""
    validate_params({
        "N": N, "LANE_NUM": LANE_NUM, "BURST_LEN": BURST_LEN,
        "K_MAX": K_MAX, "K_MAX_BITS": K_MAX_BITS,
        "RADIX_BITS": RADIX_BITS, "NUM_BINS": NUM_BINS, "HIST_W": HIST_W,
        "VAL_W": VAL_W, "ELEM_IDX_W": ELEM_IDX_W,
        "CUMSUM_BINS_PER_CY": CUMSUM_BINS_PER_CY,
        "FILTER_LANES_PER_CY": FILTER_LANES_PER_CY,
    })
    CUMSUM_CHUNKS = cumsum_num_chunks(
        num_bins=NUM_BINS, bins_per_chunk=CUMSUM_BINS_PER_CY,
    )
    FILTER_CHUNKS = filter_num_chunks(
        lane_num=LANE_NUM, lanes_per_chunk=FILTER_LANES_PER_CY,
    )

    cd = domain.clock_domain
    BUS_W = LANE_NUM * VAL_W                            # 4096 for 128 × 32
    ADDR_W = max(1, (BURST_LEN - 1).bit_length())        # 3 for BURST_LEN=8
    MASK_W = N                                          # 1024-bit total mask
    POS_W = max(1, (LANE_NUM).bit_length())              # 8-bit position counter (0..128)
    OFFSET_W = max(1, (LANE_NUM - 1).bit_length())       # 7-bit barrel offset
    FILTER_CHUNK_W = max(1, (FILTER_CHUNKS - 1).bit_length())
    filter_chunk_last = u(FILTER_CHUNK_W, FILTER_CHUNKS - 1)
    zero_filter_chunk = u(FILTER_CHUNK_W, 0)

    # ────────────────────────────────────────────────────────────
    # §1 Ports (arch §3.1)
    # ────────────────────────────────────────────────────────────
    cfg_topk = m.input("cfg_topk", width=K_MAX_BITS)
    in_req   = m.input("in_req",   width=1)
    in_data  = m.input("in_data",  width=BUS_W)

    # K=0 → 1 clamp (arch §3.1 / §10)
    zero_k = u(K_MAX_BITS, 0)
    one_k  = u(K_MAX_BITS, 1)
    k_is_zero = cfg_topk == zero_k
    cfg_topk_eff = one_k if k_is_zero else cfg_topk

    # ────────────────────────────────────────────────────────────
    # §2 FSM state regs
    # ────────────────────────────────────────────────────────────
    main_phase   = m.out("main_phase",  domain=cd, width=PH_W,        init=u(PH_W, PH_IDLE))
    sub_step     = m.out("sub_step",    domain=cd, width=SUBSTEP_W,    init=u(SUBSTEP_W, 0))
    cur_round    = m.out("cur_round",   domain=cd, width=2,           init=u(2, 0))
    recv_active  = m.out("recv_active", domain=cd, width=1,           init=u(1, 0))
    loaded_K     = m.out("loaded_K",    domain=cd, width=K_MAX_BITS,   init=u(K_MAX_BITS, 1))
    bottomK      = m.out("bottomK",     domain=cd, width=HIST_W,      init=u(HIST_W, 0))

    main_phase_q = main_phase.out()
    sub_step_q   = sub_step.out()
    cur_round_q  = cur_round.out()
    recv_active_q = recv_active.out()
    loaded_K_q   = loaded_K.out()
    bottomK_q    = bottomK.out()

    in_idle = main_phase_q == u(PH_W, PH_IDLE)
    in_load = main_phase_q == u(PH_W, PH_LOAD)
    in_hist = main_phase_q == u(PH_W, PH_HIST)
    in_cum  = main_phase_q == u(PH_W, PH_CUMSUM)
    in_mask = main_phase_q == u(PH_W, PH_MASK)
    in_filter = main_phase_q == u(PH_W, PH_FILTER)
    in_wait_out = main_phase_q == u(PH_W, PH_WAIT_OUT)
    in_drain = main_phase_q == u(PH_W, PH_DRAIN)

    addr_last = u(ADDR_W, BURST_LEN - 1)
    cum_last = u(SUBSTEP_W, CUMSUM_CHUNKS - 1)
    sub_step_lo = sub_step_q.slice(lsb=0, width=ADDR_W)
    at_beat_last = sub_step_lo == addr_last
    at_cum_last = in_cum & (sub_step_q == cum_last)
    is_last_round = cur_round_q == u(2, 3)

    # ────────────────────────────────────────────────────────────
    # §3 Receive sub-FSM (arch §5.2 t=0..8)
    # ────────────────────────────────────────────────────────────
    # recv_active goes high on the cycle AFTER in_req, holds for 8 cycles
    # while the burst arrives, then drops.
    recv_clear = recv_active_q & at_beat_last & in_load
    next_recv_active = in_req | (recv_active_q & ~recv_clear)
    recv_active.set(next_recv_active)

    # K latched on in_req cycle (stays stable across the whole task).
    loaded_K.set(cfg_topk_eff, when=in_req)

    # ────────────────────────────────────────────────────────────
    # §4 data_sram (8 rows × 4096 bit) — write side LOAD, read side per phase
    # ────────────────────────────────────────────────────────────
    in_lanes = unpack_lanes(in_data, lane_w=VAL_W, lanes=LANE_NUM)
    key_lanes_in = [fp32_to_sortable_key_hw(m, x) for x in in_lanes]
    sram_wdata = pack_lanes_lsb_first(m, key_lanes_in)

    wstrb_w = (BUS_W + 7) // 8
    wstrb_const = (in_data | ~in_data).slice(lsb=0, width=wstrb_w)
    sram_ren = in_req | ~in_req

    sram_wvalid = recv_active_q & in_load

    # sync_mem has 1-cycle read latency: sram_rdata at cycle t reflects
    # mem[raddr at cycle (t-1)]. So during a scanning phase, raddr is one
    # beat AHEAD of the beat we want to consume next cycle.
    #
    #   beat to consume at cycle t  = sub_step_lo at t
    #   raddr we need at cycle t-1  = sub_step_lo at t
    # The previous-cycle setup is provided by the cycle BEFORE the scan
    # starts (non-scanning phases drive raddr=0 so the first scan cycle
    # naturally consumes mem[0]). Inside the scan, raddr at cycle t
    # = (sub_step_lo + 1) mod BURST_LEN so cycle t+1 sees the right beat.
    scanning_phase = in_hist | in_mask | in_filter
    raddr_inc = (sub_step_lo + u(ADDR_W, 1)
                 ).slice(lsb=0, width=ADDR_W)
    raddr = raddr_inc if scanning_phase else u(ADDR_W, 0)
    waddr = sub_step_lo

    sram_rdata = m.sync_mem(
        cd.clk, cd.rst,
        ren=sram_ren,
        raddr=raddr,
        wvalid=sram_wvalid,
        waddr=waddr,
        wdata=sram_wdata,
        wstrb=wstrb_const,
        depth=BURST_LEN,
        name="data_sram",
    )

    # data_sram read lanes (each 32-bit sortable key)
    sram_key_lanes = unpack_lanes(sram_rdata, lane_w=VAL_W, lanes=LANE_NUM)

    # ────────────────────────────────────────────────────────────
    # §5 histogram_engine (hist_accum + cumsum + mask)
    # ────────────────────────────────────────────────────────────
    load_start = in_load & (sub_step_q == u(SUBSTEP_W, 0))
    cum_start = in_cum & (sub_step_q == u(SUBSTEP_W, 0))
    cum_done = at_cum_last

    # --- mask submodule (state + row select) ---
    mask_reg = m.out("mask_reg", domain=cd, width=MASK_W, init=u(MASK_W, (1 << MASK_W) - 1))
    mask_reg_q = mask_reg.out()
    _, mask_row_lanes = mask_row_lanes_for_beat(
        m, mask_reg_q, sub_step_lo, burst_len=BURST_LEN, lane_num=LANE_NUM,
    )

    # --- hist_accum submodule ---
    cycle_counts = hist_cycle_counts(
        m, sram_key_lanes, mask_row_lanes, cur_round_q,
        num_bins=NUM_BINS, count_width=POS_W,
    )
    hist_regs = [
        m.out(f"hist_b{b}", domain=cd, width=HIST_W, init=u(HIST_W, 0))
        for b in range(NUM_BINS)
    ]
    hist_clear = in_hist & (sub_step_q == u(SUBSTEP_W, 0))
    hist_accumulate = in_hist
    zero_hist = u(HIST_W, 0)
    for b in range(NUM_BINS):
        cur = hist_regs[b].out()
        cnt_ext = cycle_counts[b]
        if cnt_ext.width < HIST_W:
            pad = u(HIST_W - cnt_ext.width, 0)
            cnt_hw = cat(pad, cnt_ext)
        else:
            cnt_hw = cnt_ext.slice(lsb=0, width=HIST_W)
        added = (cur + cnt_hw).slice(lsb=0, width=HIST_W)
        next_h = zero_hist if hist_clear else (added if hist_accumulate else cur)
        hist_regs[b].set(next_h)
    hist_now = [r.out() for r in hist_regs]

    # --- cumsum submodule (pipelined: CUMSUM_BINS_PER_CY bins per cycle) ---
    cum_running = m.out(
        "cum_running", domain=cd, width=HIST_W + 1, init=u(HIST_W + 1, 0),
    )
    cum_target_found = m.out(
        "cum_target_found", domain=cd, width=1, init=u(1, 0),
    )
    cum_target_bin = m.out(
        "cum_target_bin", domain=cd, width=8, init=u(8, 0),
    )
    cum_prev_cum = m.out(
        "cum_prev_cum", domain=cd, width=HIST_W, init=u(HIST_W, 0),
    )
    cum_hist_at_target = m.out(
        "cum_hist_at_target", domain=cd, width=HIST_W, init=u(HIST_W, 0),
    )
    cum_running_q = cum_running.out()
    cum_target_found_q = cum_target_found.out()
    cum_target_bin_q = cum_target_bin.out()
    cum_prev_cum_q = cum_prev_cum.out()
    cum_hist_at_target_q = cum_hist_at_target.out()

    running_out, found_out, target_bin_out, prev_cum_out, hist_at_target_out = (
        cumsum_chunk_step(
            m, hist_now, bottomK_q, cum_running_q, sub_step_q,
            bins_per_chunk=CUMSUM_BINS_PER_CY,
            num_chunks=CUMSUM_CHUNKS,
            bin_width=8,
            hist_width=HIST_W,
            target_found_q=cum_target_found_q,
            target_bin_q=cum_target_bin_q,
            prev_cum_q=cum_prev_cum_q,
            hist_at_target_q=cum_hist_at_target_q,
        )
    )
    zero_found = u(1, 0)
    zero_running = u(HIST_W + 1, 0)
    zero_hist_w = u(HIST_W, 0)
    cum_running.set(
        zero_running if cum_start else (running_out if in_cum else cum_running_q)
    )
    cum_target_found.set(
        zero_found if cum_start else (found_out if in_cum else cum_target_found_q)
    )
    cum_target_bin.set(
        target_bin_out if in_cum else cum_target_bin_q
    )
    cum_prev_cum.set(
        prev_cum_out if in_cum else cum_prev_cum_q
    )
    cum_hist_at_target.set(
        hist_at_target_out if in_cum else cum_hist_at_target_q
    )

    target_bin_now = target_bin_out
    prev_cum_now = prev_cum_out
    hist_at_tb_now = hist_at_target_out
    target_bin_lat = [
        m.out(f"target_bin_lat{r}", domain=cd, width=8, init=u(8, 0))
        for r in range(4)
    ]
    for r in range(4):
        when_r = cum_done & (cur_round_q == u(2, r))
        target_bin_lat[r].set(target_bin_now, when=when_r)

    bottomK_init_load = (u(HIST_W, N)
                         - cfg_topk_eff
                         + u(HIST_W, 1)
                         ).slice(lsb=0, width=HIST_W)
    bottomK_dec = (bottomK_q - prev_cum_now
                   ).slice(lsb=0, width=HIST_W)
    next_bottomK = (bottomK_init_load if load_start
                    else (bottomK_dec if cum_done else bottomK_q))
    bottomK.set(next_bottomK)

    # --- mask submodule (refine + register update) ---
    tb_for_mask = target_bin_for_mask(
        m, [target_bin_lat[r].out() for r in range(4)], cur_round_q,
    )
    new_mask_lane_bits = mask_refine_lane_bits(
        m, sram_key_lanes, prior_round_idx(m, cur_round_q),
        tb_for_mask, mask_row_lanes,
    )
    mask_next_combined = mask_row_replace(
        m, mask_reg_q, new_mask_lane_bits, sub_step_lo,
        burst_len=BURST_LEN, lane_num=LANE_NUM,
    )
    mask_all_ones = u(MASK_W, (1 << MASK_W) - 1)
    next_mask = (
        mask_all_ones if load_start
        else (mask_next_combined if in_mask else mask_reg_q)
    )
    mask_reg.set(next_mask)

    # ────────────────────────────────────────────────────────────
    # §11 kth_key composition (MSB-first)
    # ────────────────────────────────────────────────────────────
    kth_key = kth_compose([target_bin_lat[r].out() for r in range(4)])
    assert kth_key.width == VAL_W

    # ────────────────────────────────────────────────────────────
    # §12 Single-pass FILTER (GT ∪ EQ_kept) — see §4.2.9 arch.md
    #
    # eq_keep (= K − gt_count) is derived at the last CUMSUM chunk of round 3
    # from (cum_hist_at_target, bottomK_3, prev_cum_3) and latched into eq_remain.
    #
    # Prefix sums (EQ positions + cap positions) and compact / obuf write all
    # run per chunk (FILTER_LANES_PER_CY lanes per PH_FILTER cycle).
    # ────────────────────────────────────────────────────────────
    filter_chunk = m.out(
        "filter_chunk", domain=cd, width=FILTER_CHUNK_W, init=zero_filter_chunk,
    )
    filter_chunk_q = filter_chunk.out()
    at_filter_chunk_last = filter_chunk_q == filter_chunk_last
    at_filter_beat_complete = at_beat_last & at_filter_chunk_last

    eq_ps_running = m.out(
        "eq_ps_running", domain=cd, width=POS_W + 1, init=u(POS_W + 1, 0),
    )
    cap_ps_running = m.out(
        "cap_ps_running", domain=cd, width=POS_W + 1, init=u(POS_W + 1, 0),
    )
    eq_ps_running_q = eq_ps_running.out()
    cap_ps_running_q = cap_ps_running.out()
    zero_ps_run = u(POS_W + 1, 0)
    filter_chunk_start = in_filter & (filter_chunk_q == zero_filter_chunk)
    eq_run_in = zero_ps_run if filter_chunk_start else eq_ps_running_q
    cap_run_in = zero_ps_run if filter_chunk_start else cap_ps_running_q

    chunk_key_lanes = filter_chunk_lanes(
        m, sram_key_lanes, filter_chunk_q,
        lanes_per_chunk=FILTER_LANES_PER_CY,
        num_chunks=FILTER_CHUNKS,
    )
    gt_flags_chunk = filter_pass_lanes(
        m, chunk_key_lanes, kth_key, pass_eq=False,
    )
    eq_flags_chunk = filter_pass_lanes(
        m, chunk_key_lanes, kth_key, pass_eq=True,
    )

    eq_positions_chunk, eq_running_out = prefix_sum_chunk(
        m, eq_flags_chunk, eq_run_in, pos_width=POS_W + 1,
    )

    # eq_remain: holds eq_keep at start of FILTER, decremented per beat.
    eq_remain = m.out("eq_remain", domain=cd, width=K_MAX_BITS, init=u(K_MAX_BITS, 0))
    eq_remain_q = eq_remain.out()

    eq_kept_chunk = []
    for i in range(FILTER_LANES_PER_CY):
        pos_w = eq_positions_chunk[i].width
        pos_ext = (eq_positions_chunk[i] if pos_w >= K_MAX_BITS
                   else cat(u(K_MAX_BITS - pos_w, 0), eq_positions_chunk[i]))
        within = pos_ext <= eq_remain_q
        eq_kept_chunk.append(eq_flags_chunk[i] & within)

    pred_chunk = [
        gt_flags_chunk[i] | eq_kept_chunk[i]
        for i in range(FILTER_LANES_PER_CY)
    ]
    cap_positions_chunk, cap_running_out = prefix_sum_chunk(
        m, pred_chunk, cap_run_in, pos_width=POS_W + 1,
    )

    eq_ps_running.set(
        eq_running_out if in_filter else eq_ps_running_q
    )
    cap_ps_running.set(
        cap_running_out if in_filter else cap_ps_running_q
    )

    filter_chunk_inc = (filter_chunk_q + u(FILTER_CHUNK_W, 1)
                        ).slice(lsb=0, width=FILTER_CHUNK_W)
    next_filter_chunk = (
        zero_filter_chunk if ~in_filter
        else (zero_filter_chunk if at_filter_chunk_last else filter_chunk_inc)
    )
    filter_chunk.set(next_filter_chunk)

    count_chunk = (cap_running_out - cap_run_in
                   ).slice(lsb=0, width=POS_W + 1)
    eq_taken_chunk = popcount_bits(m, eq_kept_chunk, out_width=K_MAX_BITS)

    eq_taken_beat = m.out(
        "eq_taken_beat", domain=cd, width=K_MAX_BITS, init=u(K_MAX_BITS, 0),
    )
    eq_taken_beat_q = eq_taken_beat.out()
    eq_taken_beat_run = (eq_taken_beat_q + eq_taken_chunk
                         ).slice(lsb=0, width=K_MAX_BITS)
    eq_taken_beat.set(
        u(K_MAX_BITS, 0) if filter_chunk_start
        else (eq_taken_beat_run if in_filter else eq_taken_beat_q)
    )

    # ────────────────────────────────────────────────────────────
    # §14 Compact within chunk → dense local slots [0 .. FILTER_LANES_PER_CY)
    # ────────────────────────────────────────────────────────────
    chunk_fp32_vals = [
        sortable_key_to_fp32_hw(m, k) for k in chunk_key_lanes
    ]
    sub_step_lo_ext = (sub_step_lo if sub_step_lo.width >= VAL_W
                       else cat(u(VAL_W - sub_step_lo.width, 0), sub_step_lo))
    sub_step_base = (sub_step_lo_ext
                     * u(VAL_W, LANE_NUM)
                     ).slice(lsb=0, width=VAL_W)
    filter_chunk_ext = (filter_chunk_q if filter_chunk_q.width >= VAL_W
                        else cat(u(VAL_W - filter_chunk_q.width, 0), filter_chunk_q))
    chunk_lane_base = (filter_chunk_ext
                       * u(VAL_W, FILTER_LANES_PER_CY)
                       ).slice(lsb=0, width=VAL_W)
    chunk_elem_idx_vals = []
    for i in range(FILTER_LANES_PER_CY):
        chunk_elem_idx_vals.append(
            (sub_step_base
             + chunk_lane_base
             + u(VAL_W, i)
             ).slice(lsb=0, width=VAL_W)
        )

    zero_val = u(VAL_W, 0)
    compact_vals: list = []
    compact_idxs: list = []
    pos_w = POS_W + 1
    for p in range(FILTER_LANES_PER_CY):
        global_pos = (cap_run_in + u(pos_w, p + 1)).slice(lsb=0, width=pos_w)
        v_acc = zero_val
        i_acc = zero_val
        for i in range(FILTER_LANES_PER_CY):
            match = (cap_positions_chunk[i] == global_pos) & pred_chunk[i]
            v_pick = chunk_fp32_vals[i] if match else zero_val
            i_pick = chunk_elem_idx_vals[i] if match else zero_val
            v_acc = v_acc | v_pick
            i_acc = i_acc | i_pick
        compact_vals.append(v_acc)
        compact_idxs.append(i_acc)

    # ────────────────────────────────────────────────────────────
    # §15 Output_buf write side: direct per-chunk writes (≤16 targets)
    # ────────────────────────────────────────────────────────────
    # output_buf as 8 rows × LANE_NUM lanes × 32-bit regs (val + idx).
    out_buf_val = [
        [m.out(f"obuf_v_r{r}_l{l}", domain=cd, width=VAL_W, init=u(VAL_W, 0))
         for l in range(LANE_NUM)]
        for r in range(BURST_LEN)
    ]
    out_buf_idx = [
        [m.out(f"obuf_i_r{r}_l{l}", domain=cd, width=VAL_W, init=u(VAL_W, 0))
         for l in range(LANE_NUM)]
        for r in range(BURST_LEN)
    ]

    # wptr (11-bit) tracks total elements written so far this task.
    wptr = m.out("wptr", domain=cd, width=K_MAX_BITS, init=u(K_MAX_BITS, 0))
    wptr_q = wptr.out()
    # offset = wptr % LANE_NUM (low OFFSET_W bits); row = wptr // LANE_NUM
    offset = wptr_q.slice(lsb=0, width=OFFSET_W)
    wptr_row_full = wptr_q.slice(lsb=OFFSET_W, width=K_MAX_BITS - OFFSET_W)
    wptr_row = wptr_row_full.slice(lsb=0, width=ADDR_W)

    # For each local compact slot k, write directly to obuf[row_k][lane_k]
    # where (row_k, lane_k) = decode(wptr + k).  O(FILTER_LANES_PER_CY) write
    # decoders instead of scanning all LANE_NUM × BURST_LEN cells.
    filt_writing = in_filter
    off_ext = (offset if offset.width >= OFFSET_W + 1
               else cat(u(1, 0), offset))
    write_row_k: list = []
    write_lane_k: list = []
    write_valid_k: list = []
    for k in range(FILTER_LANES_PER_CY):
        sum_k = (off_ext + u(OFFSET_W + 1, k)).slice(lsb=0, width=OFFSET_W + 1)
        lane_k = sum_k.slice(lsb=0, width=OFFSET_W)
        row_carry = sum_k.slice(lsb=OFFSET_W, width=1)
        row_carry_ext = (row_carry if row_carry.width >= ADDR_W
                         else cat(u(ADDR_W - row_carry.width, 0), row_carry))
        row_k = (wptr_row + row_carry_ext).slice(lsb=0, width=ADDR_W)
        write_row_k.append(row_k)
        write_lane_k.append(lane_k)
        write_valid_k.append(filt_writing & (u(POS_W + 1, k) < count_chunk))

    for r in range(BURST_LEN):
        r_const = u(ADDR_W, r)
        for l in range(LANE_NUM):
            l_const = u(OFFSET_W, l)
            pick_v = zero_val
            pick_i = zero_val
            any_en = u(1, 0)
            for k in range(FILTER_LANES_PER_CY):
                en = (write_valid_k[k]
                      & (write_row_k[k] == r_const)
                      & (write_lane_k[k] == l_const))
                any_en = any_en | en
                pick_v = pick_v | (compact_vals[k] if en else zero_val)
                pick_i = pick_i | (compact_idxs[k] if en else zero_val)
            out_buf_val[r][l].set(pick_v, when=any_en)
            out_buf_idx[r][l].set(pick_i, when=any_en)

    # wptr update
    count_ext = (count_chunk if count_chunk.width >= K_MAX_BITS
                 else cat(u(K_MAX_BITS - count_chunk.width, 0),
                          count_chunk))
    wptr_next = (wptr_q + count_ext
                 ).slice(lsb=0, width=K_MAX_BITS)
    # reset wptr on LOAD start
    next_wptr = (u(K_MAX_BITS, 0) if load_start
                 else (wptr_next if filt_writing else wptr_q))
    wptr.set(next_wptr)

    # eq_remain update.
    #
    # At the CUMSUM cycle of the LAST radix round, derive eq_keep from
    # the round-3 histogram + bottomK state and latch into eq_remain.
    # Then per FILTER beat (last chunk), decrement by # EQ taken across the beat.
    eq_taken_beat_now = eq_taken_beat_run

    # bottomK_4 = bottomK_q - prev_cum_now (using bottomK_dec computed in §9).
    # eq_keep = hist_at_tb - bottomK_4 + 1 (hist_at_tb latched during cumsum scan).
    hist_at_tb_ext = (hist_at_tb_now if hist_at_tb_now.width >= K_MAX_BITS
                      else cat(u(K_MAX_BITS - hist_at_tb_now.width, 0),
                               hist_at_tb_now))
    bottomK4_ext = (bottomK_dec if bottomK_dec.width >= K_MAX_BITS
                    else cat(u(K_MAX_BITS - bottomK_dec.width, 0),
                             bottomK_dec))
    eq_keep_next = ((hist_at_tb_ext
                     - bottomK4_ext
                     + u(K_MAX_BITS, 1)
                     ).slice(lsb=0, width=K_MAX_BITS))

    latch_eq_keep = cum_done & is_last_round
    eq_remain_dec = (eq_remain_q - eq_taken_beat_now
                     ).slice(lsb=0, width=K_MAX_BITS)
    next_eq_remain = (eq_keep_next if latch_eq_keep
                      else (eq_remain_dec if (in_filter & at_filter_chunk_last)
                            else eq_remain_q))
    eq_remain.set(next_eq_remain)

    # ────────────────────────────────────────────────────────────
    # §16 Send sub-FSM (DRAIN: 8-beat burst)
    # ────────────────────────────────────────────────────────────
    send_cnt = m.out("send_cnt", domain=cd, width=ADDR_W, init=u(ADDR_W, 0))
    send_cnt_q = send_cnt.out()
    drain_inc = (send_cnt_q + u(ADDR_W, 1)
                 ).slice(lsb=0, width=ADDR_W)
    drain_at_last = send_cnt_q == addr_last
    next_send_cnt = (drain_inc if in_drain
                     else u(ADDR_W, 0))
    send_cnt.set(next_send_cnt)

    # out_value / out_index_data: select row send_cnt from output_buf
    drain_row_v_options = [
        pack_lanes_lsb_first(m, [out_buf_val[r][l].out() for l in range(LANE_NUM)])
        for r in range(BURST_LEN)
    ]
    drain_row_i_options = [
        pack_lanes_lsb_first(m, [out_buf_idx[r][l].out() for l in range(LANE_NUM)])
        for r in range(BURST_LEN)
    ]
    out_value_bus = mux_phase(m, send_cnt_q, drain_row_v_options, default=drain_row_v_options[0])
    out_index_bus = mux_phase(m, send_cnt_q, drain_row_i_options, default=drain_row_i_options[0])

    # out_valid_mask per beat:
    #   total_count = wptr_q (latched after filter)
    #   beat = send_cnt
    #   used = total - beat * LANE_NUM (clamped to [0, LANE_NUM])
    #   mask = (1 << used) - 1
    total_count = m.out("total_count", domain=cd, width=K_MAX_BITS, init=u(K_MAX_BITS, 0))
    total_count_q = total_count.out()
    # latch wptr_next (this cycle's final write count) into total_count when
    # leaving FILTER → WAIT_OUT.  Uses wptr_next, not wptr_q, because the
    # filter exit condition is checked against wptr_next inline.
    entering_wait_out = in_filter & (
        (wptr_next == loaded_K_q) | at_filter_beat_complete
    )
    total_count.set(wptr_next, when=entering_wait_out)

    # Per beat, mask:
    base_per_beat = []
    for beat in range(BURST_LEN):
        base = (u(K_MAX_BITS, beat * LANE_NUM))
        # used = clamp(total - base, 0, LANE_NUM)
        # Compute as: (total > base) ? min(total - base, LANE_NUM) : 0
        gt_base = total_count_q > base
        diff = (total_count_q - base).slice(lsb=0, width=K_MAX_BITS)
        ge_lane = diff >= u(K_MAX_BITS, LANE_NUM)
        used_low = diff.slice(lsb=0, width=POS_W + 1)
        # Build mask wire: ge_lane → all-1s; else (1 << used_low) - 1
        # For each lane bit b in [0, LANE_NUM): mask_bit_b = (b < used_low)
        bits_for_beat = []
        for b in range(LANE_NUM):
            b_const = u(K_MAX_BITS, b)
            within = b_const < diff
            bit = within & gt_base
            bits_for_beat.append(bit)
        beat_mask = pack_lanes_lsb_first(m, bits_for_beat)    # lane 0 in LSB
        base_per_beat.append(beat_mask)
    out_valid_mask_bus = mux_phase(m, send_cnt_q, base_per_beat, default=base_per_beat[0])

    # out_req asserted for 1 cycle when entering DRAIN (or when in WAIT_OUT cycle).
    # Simplest: out_req = in_wait_out (asserts for 1 cy before DRAIN starts).
    out_req_wire = in_wait_out

    # ────────────────────────────────────────────────────────────
    # §17 main_phase FSM transitions
    # ────────────────────────────────────────────────────────────
    next_main_phase = u(PH_W, PH_IDLE)
    next_sub_step = u(SUBSTEP_W, 0)
    next_cur_round = cur_round_q

    sub_step_inc = (sub_step_q + u(SUBSTEP_W, 1)
                    ).slice(lsb=0, width=SUBSTEP_W)
    sub_step_zero = u(SUBSTEP_W, 0)
    zero_round = u(2, 0)
    round_inc = (cur_round_q + u(2, 1)
                 ).slice(lsb=0, width=2)

    # State transition logic — sequential nested ternaries to keep JIT happy.
    # IDLE:    on in_req → LOAD
    # LOAD:    8 cy; on at_beat_last → HIST (round 0)
    # HIST:    8 cy; on at_beat_last → CUMSUM
    # CUMSUM:  CUMSUM_CHUNKS cy; → MASK if !is_last_round else FILTER
    # MASK:    8 cy; on at_beat_last → HIST (next round)
    # FILTER: 1..8 beats × FILTER_CHUNKS cy; exit when wptr_next == K or last beat+chunk
    # WAIT_OUT:1 cy → DRAIN (assert out_req)
    # DRAIN:   8 cy; on drain_at_last → IDLE
    filter_done = (wptr_next == loaded_K_q) | at_filter_beat_complete

    # Build next_main_phase via cascading ternaries.
    np_from_drain = (u(PH_W, PH_IDLE) if drain_at_last
                     else u(PH_W, PH_DRAIN))
    np_from_wait_out = u(PH_W, PH_DRAIN)
    np_from_filter = (u(PH_W, PH_WAIT_OUT) if filter_done
                      else u(PH_W, PH_FILTER))
    np_from_mask = (u(PH_W, PH_HIST) if at_beat_last
                    else u(PH_W, PH_MASK))
    np_from_cum = (u(PH_W, PH_FILTER) if is_last_round
                   else u(PH_W, PH_MASK))
    np_from_cum_hold = np_from_cum if at_cum_last else u(PH_W, PH_CUMSUM)
    np_from_hist = (u(PH_W, PH_CUMSUM) if at_beat_last
                    else u(PH_W, PH_HIST))
    np_from_load = (u(PH_W, PH_HIST) if at_beat_last
                    else u(PH_W, PH_LOAD))
    np_from_idle = (u(PH_W, PH_LOAD) if in_req
                    else u(PH_W, PH_IDLE))

    next_main_phase = (
        np_from_idle if in_idle
        else (np_from_load if in_load
        else (np_from_hist if in_hist
        else (np_from_cum_hold if in_cum
        else (np_from_mask if in_mask
        else (np_from_filter if in_filter
        else (np_from_wait_out if in_wait_out
        else (np_from_drain if in_drain
        else u(PH_W, PH_IDLE)))))))))

    main_phase.set(next_main_phase)

    # sub_step: 0..7 in LOAD/HIST/MASK; 0..CUMSUM_CHUNKS-1 in CUMSUM;
    #           advances once per FILTER beat (on last chunk of beat).
    in_count_phase = in_load | in_hist | in_mask | in_cum
    in_filter_count = in_filter
    at_phase_last = at_cum_last if in_cum else at_beat_last
    np_sub_from_filter = (
        sub_step_zero if at_filter_beat_complete
        else (sub_step_inc if at_filter_chunk_last else sub_step_q)
    )
    np_sub = (
        np_sub_from_filter if in_filter_count
        else ((sub_step_zero if at_phase_last else sub_step_inc)
              if in_count_phase
              else sub_step_zero)
    )
    sub_step.set(np_sub)

    # cur_round:
    #   reset to 0 on LOAD start (load_start)
    #   increment at CUMSUM end (cum_done & !is_last_round)
    incr_round = cum_done & ~is_last_round
    next_cur_round = (zero_round if load_start
                      else (round_inc if incr_round else cur_round_q))
    cur_round.set(next_cur_round)

    # ────────────────────────────────────────────────────────────
    # §18 Outputs
    # ────────────────────────────────────────────────────────────
    status_busy_wire = recv_active_q | (~in_idle) | (out_req_wire)
    m.output("status_busy",    status_busy_wire)
    m.output("out_req",        out_req_wire)
    m.output("out_value",      out_value_bus)
    m.output("out_index_data", out_index_bus)
    m.output("out_valid_mask", out_valid_mask_bus)

    _meta.update({
        "N": N, "LANE_NUM": LANE_NUM, "BURST_LEN": BURST_LEN,
        "K_MAX": K_MAX, "K_MAX_BITS": K_MAX_BITS,
        "ADDR_W": ADDR_W, "BUS_W": BUS_W,
        "NUM_BINS": NUM_BINS,
        "CUMSUM_BINS_PER_CY": CUMSUM_BINS_PER_CY,
        "CUMSUM_CHUNKS": CUMSUM_CHUNKS,
        "FILTER_LANES_PER_CY": FILTER_LANES_PER_CY,
        "FILTER_CHUNKS": FILTER_CHUNKS,
    })


build.__pycircuit_name__ = "topk_histogram"


def _build_params() -> dict:
    """Filter DEFAULT_PARAMS to kwargs ``build()`` accepts."""
    import inspect
    sig = inspect.signature(build)
    accepted = {p for p in sig.parameters if p not in ("m", "domain")}
    return {k: v for k, v in DEFAULT_PARAMS.items() if k in accepted}


if __name__ == "__main__":
    print("Building topk_histogram MLIR...")
    circuit = compile_cycle_aware(
        build,
        name="topk_histogram",
        **_build_params(),
    )
    mlir = circuit.emit_mlir()
    print(f"  meta = {_meta}")
    print(f"  MLIR length = {len(mlir)} chars")
