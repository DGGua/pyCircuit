"""Top-K Histogram accelerator — top-level pycircuit `build` module.

Implements the radix-select Top-K pipeline from
[designs/topk-histogram/arch.md](designs/topk-histogram/arch.md).

Design choices (v1):
  - 256-bin histogram engine, 128-lane popcount per bin per cycle
  - Combinational cumsum + priority encoder (no pipeline reg) — the CUMSUM
    phase still spends 1 cy so the FSM is regular and arch-compatible
  - 1024-bit mask reg, updated by a separate 8-cy MASK_UPD pass per round
  - Filter writes into a flat 1024-lane output_buf (val + idx, 32-bit each)
    via prefix-sum compact + barrel rotate + per-(row,lane) reg mux

State allocations (m.out / m.sync_mem) are all inline because plain helpers
trigger the JIT structural-metrics check on `state_call_count > 0`.
Combinational helpers (no state) live in `datapath.py` / `histogram.py` /
`filter_output.py`.

Removed-API note: inside `@module build`, never call `.select(...)` (PYC430);
use ternary `a if c else b` instead. Inside helpers (regular Python),
`.select` is fine because the JIT dispatches to `fn(*args)` and never scans
the helper's AST.
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
    byte_select_lane,
    kth_compose,
    mask_row_update,
    mux_phase,
    pack_lanes_lsb_first,
    unpack_lanes,
)
from histogram import (
    cumsum_threshold,
    hist_cycle_counts,
)
from filter_output import (
    filter_pass_lanes,
    prefix_sum_lanes,
)


_meta: dict = {}


# Phase encoding (main_phase)
#
# Single-pass FILTER: GT and EQ predicates are evaluated together each cycle.
# eq_keep is precomputed at the end of CUMSUM round 3 from hist + bottomK
# (see §12 below), so we don't need a separate FILT_GT pass to learn gt_count.
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

NUM_BINS = 256
RADIX_BITS = 8


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
) -> None:
    """arch.md §3.1 contract: 1024 fp32 in 8-beat burst → top-K out 8-beat burst."""
    validate_params({
        "N": N, "LANE_NUM": LANE_NUM, "BURST_LEN": BURST_LEN,
        "K_MAX": K_MAX, "K_MAX_BITS": K_MAX_BITS,
        "RADIX_BITS": 8, "HIST_W": HIST_W, "VAL_W": VAL_W,
        "ELEM_IDX_W": ELEM_IDX_W,
    })

    cd = domain.clock_domain
    BUS_W = LANE_NUM * VAL_W                            # 4096 for 128 × 32
    ADDR_W = max(1, (BURST_LEN - 1).bit_length())        # 3 for BURST_LEN=8
    MASK_W = N                                          # 1024-bit total mask
    POS_W = max(1, (LANE_NUM).bit_length())              # 8-bit position counter (0..128)
    OFFSET_W = max(1, (LANE_NUM - 1).bit_length())       # 7-bit barrel offset

    # ────────────────────────────────────────────────────────────
    # §1 Ports (arch §3.1)
    # ────────────────────────────────────────────────────────────
    cfg_topk = m.input("cfg_topk", width=K_MAX_BITS)
    in_req   = m.input("in_req",   width=1)
    in_data  = m.input("in_data",  width=BUS_W)

    # K=0 → 1 clamp (arch §3.1 / §10)
    zero_k = m.const(0, width=K_MAX_BITS)
    one_k  = m.const(1, width=K_MAX_BITS)
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

    in_idle = main_phase_q == m.const(PH_IDLE, width=PH_W)
    in_load = main_phase_q == m.const(PH_LOAD, width=PH_W)
    in_hist = main_phase_q == m.const(PH_HIST, width=PH_W)
    in_cum  = main_phase_q == m.const(PH_CUMSUM, width=PH_W)
    in_mask = main_phase_q == m.const(PH_MASK, width=PH_W)
    in_filter = main_phase_q == m.const(PH_FILTER, width=PH_W)
    in_wait_out = main_phase_q == m.const(PH_WAIT_OUT, width=PH_W)
    in_drain = main_phase_q == m.const(PH_DRAIN, width=PH_W)

    addr_last = m.const(BURST_LEN - 1, width=ADDR_W)
    sub_step_lo = sub_step_q.slice(lsb=0, width=ADDR_W)
    at_beat_last = sub_step_lo == addr_last
    is_last_round = cur_round_q == m.const(3, width=2)

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
    wstrb_const = m.const((1 << wstrb_w) - 1, width=wstrb_w)

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
    raddr_inc = (sub_step_lo.as_unsigned() + m.const(1, width=ADDR_W).as_unsigned()
                 ).slice(lsb=0, width=ADDR_W)
    raddr = raddr_inc if scanning_phase else m.const(0, width=ADDR_W)
    waddr = sub_step_lo

    sram_rdata = m.sync_mem(
        cd.clk, cd.rst,
        ren=m.const(1, width=1),
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
    # §5 Mask register: 1024-bit (8 rows × 128 lanes)
    # ────────────────────────────────────────────────────────────
    mask_reg = m.out("mask_reg", domain=cd, width=MASK_W, init=u(MASK_W, (1 << MASK_W) - 1))
    mask_reg_q = mask_reg.out()
    # Current mask row matches the beat currently on sram_rdata, which is
    # `sub_step_lo` (NOT raddr — raddr is the address for the NEXT cycle's
    # rdata, see §4 above). Mux 8 row slices by sub_step_lo.
    mask_row_options = [
        mask_reg_q.slice(lsb=row * LANE_NUM, width=LANE_NUM)
        for row in range(BURST_LEN)
    ]
    mask_row = mux_phase(m, sub_step_lo, mask_row_options, default=mask_row_options[0])
    mask_row_lanes = [mask_row.slice(lsb=l, width=1) for l in range(LANE_NUM)]

    # ────────────────────────────────────────────────────────────
    # §6 Round-selected byte per lane (from current sram beat)
    # ────────────────────────────────────────────────────────────
    lane_bytes = [byte_select_lane(m, sram_key_lanes[l], cur_round_q) for l in range(LANE_NUM)]

    # ────────────────────────────────────────────────────────────
    # §7 Histogram accumulators (256 bins × HIST_W) + per-cycle count
    # ────────────────────────────────────────────────────────────
    cycle_counts = hist_cycle_counts(
        m, lane_bytes, mask_row_lanes,
        num_bins=NUM_BINS, count_width=POS_W,
    )

    hist_regs = [
        m.out(f"hist_b{b}", domain=cd, width=HIST_W, init=u(HIST_W, 0))
        for b in range(NUM_BINS)
    ]
    # Update logic: clear at start of HIST phase (sub_step==0), accumulate during HIST cycles,
    # hold otherwise.
    hist_clear = in_hist & (sub_step_q == m.const(0, width=SUBSTEP_W))
    hist_accumulate = in_hist
    zero_hist = m.const(0, width=HIST_W)
    for b in range(NUM_BINS):
        cur = hist_regs[b].out()
        cnt_ext = cycle_counts[b]
        # zero-extend count to HIST_W
        if cnt_ext.width < HIST_W:
            pad = m.const(0, width=HIST_W - cnt_ext.width)
            cnt_hw = cat(pad, cnt_ext)
        else:
            cnt_hw = cnt_ext.slice(lsb=0, width=HIST_W)
        added = (cur.as_unsigned() + cnt_hw.as_unsigned()).slice(lsb=0, width=HIST_W)
        next_h = zero_hist if hist_clear else (added if hist_accumulate else cur)
        hist_regs[b].set(next_h)

    hist_now = [r.out() for r in hist_regs]

    # ────────────────────────────────────────────────────────────
    # §8 Cumsum + priority encoder (combinational)
    # ────────────────────────────────────────────────────────────
    target_bin_now, prev_cum_now = cumsum_threshold(
        m, hist_now, bottomK_q,
        bin_width=8, hist_width=HIST_W,
    )

    # Latch target_bin per round when CUMSUM completes.
    target_bin_lat = [
        m.out(f"target_bin_lat{r}", domain=cd, width=8, init=u(8, 0))
        for r in range(4)
    ]
    cum_done = in_cum
    for r in range(4):
        when_r = cum_done & (cur_round_q == m.const(r, width=2))
        target_bin_lat[r].set(target_bin_now, when=when_r)

    # ────────────────────────────────────────────────────────────
    # §9 bottomK update at CUMSUM completion: bottomK -= prev_cum
    # ────────────────────────────────────────────────────────────
    bottomK_init_load = (m.const(N, width=HIST_W).as_unsigned()
                         - cfg_topk_eff.as_unsigned()
                         + m.const(1, width=HIST_W).as_unsigned()
                         ).slice(lsb=0, width=HIST_W)
    bottomK_dec = (bottomK_q.as_unsigned() - prev_cum_now.as_unsigned()
                   ).slice(lsb=0, width=HIST_W)
    # initialize bottomK on LOAD start (sub_step==0 in LOAD phase)
    load_start = in_load & (sub_step_q == m.const(0, width=SUBSTEP_W))
    next_bottomK = (bottomK_init_load if load_start
                    else (bottomK_dec if cum_done else bottomK_q))
    bottomK.set(next_bottomK)

    # ────────────────────────────────────────────────────────────
    # §10 Mask update phase: keep only lanes where byte == target_bin_lat[cur_round-1]
    # ────────────────────────────────────────────────────────────
    # During MASK phase, cur_round has ALREADY been incremented to the next round;
    # we apply the just-completed round's target_bin to refine the mask.
    # Build target_bin choice from target_bin_lat[0..3] indexed by cur_round-1.
    # We use cur_round as the "round we are ABOUT TO PROCESS", and the mask
    # being applied is from round (cur_round - 1). So pick target_bin_lat[cur_round - 1].
    # Implementation: index by cur_round - 1 = cur_round + 3 (mod 4) using a 4-input mux.
    # Cleaner: track which round's tb to use via a small mux on cur_round.
    # During MASK phase, cur_round in {1, 2, 3} (after round 0/1/2 CUMSUM).
    tb_for_mask = mux_phase(
        m, cur_round_q,
        [target_bin_lat[0].out(),  # cur_round==0 (unused in mask)
         target_bin_lat[0].out(),  # cur_round==1: use round-0 tb
         target_bin_lat[1].out(),  # cur_round==2: use round-1 tb
         target_bin_lat[2].out()], # cur_round==3: use round-2 tb
        default=target_bin_lat[0].out(),
    )

    # For each lane in current row: new bit = old bit AND (lane_byte == tb_for_mask)
    # lane_bytes uses cur_round byte. But mask update uses the PRIOR round's byte.
    # During MASK phase, cur_round has been bumped; lane_bytes already picks byte
    # for the new round. We need the PRIOR round's byte for mask gating.
    # Pick prior-round byte explicitly:
    prior_round = (cur_round_q.as_unsigned() - m.const(1, width=2).as_unsigned()
                   ).slice(lsb=0, width=2)
    lane_bytes_prior = [byte_select_lane(m, sram_key_lanes[l], prior_round) for l in range(LANE_NUM)]

    new_mask_lane_bits = [
        mask_row_lanes[l] & (lane_bytes_prior[l] == tb_for_mask)
        for l in range(LANE_NUM)
    ]

    # mask_reg next: during MASK phase, replace current row with new lane bits.
    # Implemented via mask_row_update helper, but the row index varies — build
    # 8 candidate next-states, mux by raddr.
    mask_next_per_row = [
        mask_row_update(
            m, mask_reg_q,
            row_idx=r,
            lane_keep=new_mask_lane_bits,
            total_rows=BURST_LEN,
            lane_num=LANE_NUM,
        )
        for r in range(BURST_LEN)
    ]
    mask_next_combined = mux_phase(m, sub_step_lo, mask_next_per_row, default=mask_reg_q)
    # On LOAD start: reinit mask to all 1s
    mask_all_ones = m.const((1 << MASK_W) - 1, width=MASK_W)
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
    # eq_keep (= K − gt_count) is derived combinationally at the end of
    # CUMSUM round 3 from (hist_3[tb_3], bottomK_3, prev_cum_3) and latched
    # into eq_remain for use by PH_FILTER.  This lets FILTER scan data_sram
    # once and stop as soon as wptr_next reaches K.
    #
    # Derivation (proof in arch.md §4.2.9):
    #     bottomK_4 = bottomK_3 - prev_cum_3
    #     eq_keep   = hist_3[tb_3] - bottomK_4 + 1
    #               = hist_3[tb_3] - bottomK_q + prev_cum_now + 1   (eval @ CUMSUM_R3)
    # ────────────────────────────────────────────────────────────
    gt_flags = filter_pass_lanes(m, sram_key_lanes, kth_key, pass_eq=False)
    eq_flags = filter_pass_lanes(m, sram_key_lanes, kth_key, pass_eq=True)

    # eq_remain: holds eq_keep at start of FILTER, decremented per cycle.
    eq_remain = m.out("eq_remain", domain=cd, width=K_MAX_BITS, init=u(K_MAX_BITS, 0))
    eq_remain_q = eq_remain.out()

    # Per-lane inclusive prefix-sum on eq_flags (1-indexed position).
    # POS_W+1 ensures we can count up to LANE_NUM=128.
    eq_positions, eq_total = prefix_sum_lanes(m, eq_flags, pos_width=POS_W + 1)

    # eq_kept[l] = eq_flags[l] & (eq_positions[l] <= eq_remain).
    # Pad eq_positions to K_MAX_BITS for the compare against eq_remain_q.
    eq_kept = []
    for l in range(LANE_NUM):
        pos_w = eq_positions[l].width
        pos_ext = (eq_positions[l] if pos_w >= K_MAX_BITS
                   else cat(m.const(0, width=K_MAX_BITS - pos_w), eq_positions[l]))
        within = pos_ext.as_unsigned() <= eq_remain_q.as_unsigned()
        eq_kept.append(eq_flags[l] & within)

    # Combined predicate (GT and EQ are mutually exclusive per lane, so '|' OK).
    pred_lanes = [gt_flags[l] | eq_kept[l] for l in range(LANE_NUM)]

    # Compaction prefix-sum on the combined predicate.
    capped_positions, capped_count_this_cy = prefix_sum_lanes(
        m, pred_lanes, pos_width=POS_W + 1,
    )
    capped_flag_lanes = pred_lanes

    # ────────────────────────────────────────────────────────────
    # §14 Compact within beat: compact[p] = lane_val[l] s.t. positions[l]==p+1
    # ────────────────────────────────────────────────────────────
    # For each output position p in [0, LANE_NUM):
    #   compact_val[p] = OR_l ( (positions[l] == p+1 & flag[l]) ? lane_val[l] : 0 )
    #   compact_idx[p] = OR_l ( (positions[l] == p+1 & flag[l]) ? lane_idx[l] : 0 )
    # Only one lane matches per p, so OR works.
    # Convert sortable keys back to fp32 for output:
    lane_fp32_vals = [sortable_key_to_fp32_hw(m, sram_key_lanes[l]) for l in range(LANE_NUM)]
    # Compute per-lane element index = sub_step_lo * LANE_NUM + l
    lane_elem_idx_vals = []
    sub_step_lo_ext = (sub_step_lo if sub_step_lo.width >= VAL_W
                       else cat(m.const(0, width=VAL_W - sub_step_lo.width), sub_step_lo))
    sub_step_base = (sub_step_lo_ext.as_unsigned()
                     * m.const(LANE_NUM, width=VAL_W).as_unsigned()
                     ).slice(lsb=0, width=VAL_W)
    for l in range(LANE_NUM):
        lane_elem_idx_vals.append(
            (sub_step_base.as_unsigned()
             + m.const(l, width=VAL_W).as_unsigned()
             ).slice(lsb=0, width=VAL_W)
        )

    zero_val = m.const(0, width=VAL_W)
    compact_vals: list = []
    compact_idxs: list = []
    pos_w = capped_positions[0].width
    for p in range(LANE_NUM):
        p_plus_1_const = m.const(p + 1, width=pos_w)
        # build OR of (match ? lane_val : 0) for each lane
        v_acc = zero_val
        i_acc = zero_val
        for l in range(LANE_NUM):
            match = (capped_positions[l] == p_plus_1_const) & capped_flag_lanes[l]
            v_pick = lane_fp32_vals[l] if match else zero_val
            i_pick = lane_elem_idx_vals[l] if match else zero_val
            v_acc = v_acc | v_pick
            i_acc = i_acc | i_pick
        compact_vals.append(v_acc)
        compact_idxs.append(i_acc)

    # ────────────────────────────────────────────────────────────
    # §15 Output_buf write side: wptr-driven barrel rotation
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

    # Barrel-rotate compact[k] → rot[l] = compact[(l - offset) mod LANE_NUM]
    rot_vals: list = []
    rot_idxs: list = []
    rot_valid: list = []  # 1 if this rot[l] is one of the count valid outputs
    count_ext_pos = capped_count_this_cy
    for l in range(LANE_NUM):
        # For each l, build option list compact[(l - r) mod LANE_NUM] for r in [0, LANE_NUM)
        opts_v: list = []
        opts_i: list = []
        for r in range(LANE_NUM):
            src = (l - r) % LANE_NUM
            opts_v.append(compact_vals[src])
            opts_i.append(compact_idxs[src])
        rv = mux_phase(m, offset, opts_v, default=opts_v[0])
        ri = mux_phase(m, offset, opts_i, default=opts_i[0])
        rot_vals.append(rv)
        rot_idxs.append(ri)
        # rot[l] is valid if its source compact index k < count_this_cy
        # k = (l - offset) mod LANE_NUM
        # Compute k as a wire:
        k_opts = [m.const((l - r) % LANE_NUM, width=POS_W) for r in range(LANE_NUM)]
        k_w = mux_phase(m, offset, k_opts, default=k_opts[0])
        rot_valid.append(k_w.as_unsigned() < count_ext_pos.as_unsigned())

    # Per (row, lane), update reg:
    # in_window_a = (row == wptr_row) & (lane >= offset) & rot_valid[lane]
    # in_window_b = (row == wptr_row + 1) & (lane < offset) & rot_valid[lane]
    # new_val = (in_window_a | in_window_b) ? rot_vals[lane] : hold
    filt_writing = in_filter
    next_row = (wptr_row.as_unsigned() + m.const(1, width=ADDR_W).as_unsigned()
                ).slice(lsb=0, width=ADDR_W)
    for r in range(BURST_LEN):
        r_const = m.const(r, width=ADDR_W)
        row_is_cur = wptr_row == r_const
        row_is_next = next_row == r_const
        for l in range(LANE_NUM):
            l_const = m.const(l, width=OFFSET_W)
            l_ge_off = l_const.as_unsigned() >= offset.as_unsigned()
            l_lt_off = l_const.as_unsigned() < offset.as_unsigned()
            in_win_a = row_is_cur & l_ge_off & rot_valid[l]
            in_win_b = row_is_next & l_lt_off & rot_valid[l]
            in_win = (in_win_a | in_win_b) & filt_writing
            cur_v = out_buf_val[r][l].out()
            cur_i = out_buf_idx[r][l].out()
            next_v = rot_vals[l] if in_win else cur_v
            next_i = rot_idxs[l] if in_win else cur_i
            out_buf_val[r][l].set(next_v)
            out_buf_idx[r][l].set(next_i)

    # wptr update
    count_ext = (capped_count_this_cy if capped_count_this_cy.width >= K_MAX_BITS
                 else cat(m.const(0, width=K_MAX_BITS - capped_count_this_cy.width),
                          capped_count_this_cy))
    wptr_next = (wptr_q.as_unsigned() + count_ext.as_unsigned()
                 ).slice(lsb=0, width=K_MAX_BITS)
    # reset wptr on LOAD start
    next_wptr = (m.const(0, width=K_MAX_BITS) if load_start
                 else (wptr_next if filt_writing else wptr_q))
    wptr.set(next_wptr)

    # eq_remain update.
    #
    # At the CUMSUM cycle of the LAST radix round, derive eq_keep from
    # the round-3 histogram + bottomK state and latch into eq_remain.
    # Then per FILTER cycle, decrement by # EQ actually taken this cycle.
    #
    # eq_taken_this_cy = min(eq_total, eq_remain)
    # (an explicit min is cheaper than a third prefix-sum over eq_kept).
    eq_total_w = eq_total.width
    eq_total_ext = (eq_total if eq_total_w >= K_MAX_BITS
                    else cat(m.const(0, width=K_MAX_BITS - eq_total_w), eq_total))
    eq_total_le_remain = eq_total_ext.as_unsigned() <= eq_remain_q.as_unsigned()
    eq_taken_this_cy = eq_total_ext if eq_total_le_remain else eq_remain_q

    # bottomK_4 = bottomK_q - prev_cum_now (using bottomK_dec computed in §9).
    # eq_keep = hist_at_tb - bottomK_4 + 1, widened to K_MAX_BITS.
    hist_at_tb_now = mux_phase(m, target_bin_now, hist_now, default=hist_now[0])
    hist_at_tb_ext = (hist_at_tb_now if hist_at_tb_now.width >= K_MAX_BITS
                      else cat(m.const(0, width=K_MAX_BITS - hist_at_tb_now.width),
                               hist_at_tb_now))
    bottomK4_ext = (bottomK_dec if bottomK_dec.width >= K_MAX_BITS
                    else cat(m.const(0, width=K_MAX_BITS - bottomK_dec.width),
                             bottomK_dec))
    eq_keep_next = ((hist_at_tb_ext.as_unsigned()
                     - bottomK4_ext.as_unsigned()
                     + m.const(1, width=K_MAX_BITS).as_unsigned()
                     ).slice(lsb=0, width=K_MAX_BITS))

    latch_eq_keep = in_cum & is_last_round
    eq_remain_dec = (eq_remain_q.as_unsigned() - eq_taken_this_cy.as_unsigned()
                     ).slice(lsb=0, width=K_MAX_BITS)
    next_eq_remain = (eq_keep_next if latch_eq_keep
                      else (eq_remain_dec if in_filter else eq_remain_q))
    eq_remain.set(next_eq_remain)

    # ────────────────────────────────────────────────────────────
    # §16 Send sub-FSM (DRAIN: 8-beat burst)
    # ────────────────────────────────────────────────────────────
    send_cnt = m.out("send_cnt", domain=cd, width=ADDR_W, init=u(ADDR_W, 0))
    send_cnt_q = send_cnt.out()
    drain_inc = (send_cnt_q.as_unsigned() + m.const(1, width=ADDR_W).as_unsigned()
                 ).slice(lsb=0, width=ADDR_W)
    drain_at_last = send_cnt_q == addr_last
    next_send_cnt = (drain_inc if in_drain
                     else m.const(0, width=ADDR_W))
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
    entering_wait_out = in_filter & ((wptr_next.as_unsigned() == loaded_K_q.as_unsigned())
                                     | at_beat_last)
    total_count.set(wptr_next, when=entering_wait_out)

    # Per beat, mask:
    base_per_beat = []
    for beat in range(BURST_LEN):
        base = (m.const(beat * LANE_NUM, width=K_MAX_BITS).as_unsigned())
        # used = clamp(total - base, 0, LANE_NUM)
        # Compute as: (total > base) ? min(total - base, LANE_NUM) : 0
        gt_base = total_count_q.as_unsigned() > base
        diff = (total_count_q.as_unsigned() - base).slice(lsb=0, width=K_MAX_BITS)
        ge_lane = diff.as_unsigned() >= m.const(LANE_NUM, width=K_MAX_BITS).as_unsigned()
        used_low = diff.slice(lsb=0, width=POS_W + 1)
        # Build mask wire: ge_lane → all-1s; else (1 << used_low) - 1
        # For each lane bit b in [0, LANE_NUM): mask_bit_b = (b < used_low)
        bits_for_beat = []
        for b in range(LANE_NUM):
            b_const = m.const(b, width=K_MAX_BITS)
            within = b_const.as_unsigned() < diff.as_unsigned()
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
    next_main_phase = m.const(PH_IDLE, width=PH_W)
    next_sub_step = m.const(0, width=SUBSTEP_W)
    next_cur_round = cur_round_q

    sub_step_inc = (sub_step_q.as_unsigned() + m.const(1, width=SUBSTEP_W).as_unsigned()
                    ).slice(lsb=0, width=SUBSTEP_W)
    sub_step_zero = m.const(0, width=SUBSTEP_W)
    zero_round = m.const(0, width=2)
    round_inc = (cur_round_q.as_unsigned() + m.const(1, width=2).as_unsigned()
                 ).slice(lsb=0, width=2)

    # State transition logic — sequential nested ternaries to keep JIT happy.
    # IDLE:    on in_req → LOAD
    # LOAD:    8 cy; on at_beat_last → HIST (round 0)
    # HIST:    8 cy; on at_beat_last → CUMSUM
    # CUMSUM:  1 cy; → MASK if !is_last_round else FILTER
    # MASK:    8 cy; on at_beat_last → HIST (next round)
    # FILTER:  1..8 cy single pass; exit when wptr_next == K or at_beat_last
    # WAIT_OUT:1 cy → DRAIN (assert out_req)
    # DRAIN:   8 cy; on drain_at_last → IDLE
    filter_done = (wptr_next.as_unsigned() == loaded_K_q.as_unsigned()) | at_beat_last

    # Build next_main_phase via cascading ternaries.
    np_from_drain = (m.const(PH_IDLE, width=PH_W) if drain_at_last
                     else m.const(PH_DRAIN, width=PH_W))
    np_from_wait_out = m.const(PH_DRAIN, width=PH_W)
    np_from_filter = (m.const(PH_WAIT_OUT, width=PH_W) if filter_done
                      else m.const(PH_FILTER, width=PH_W))
    np_from_mask = (m.const(PH_HIST, width=PH_W) if at_beat_last
                    else m.const(PH_MASK, width=PH_W))
    np_from_cum = (m.const(PH_FILTER, width=PH_W) if is_last_round
                   else m.const(PH_MASK, width=PH_W))
    np_from_hist = (m.const(PH_CUMSUM, width=PH_W) if at_beat_last
                    else m.const(PH_HIST, width=PH_W))
    np_from_load = (m.const(PH_HIST, width=PH_W) if at_beat_last
                    else m.const(PH_LOAD, width=PH_W))
    np_from_idle = (m.const(PH_LOAD, width=PH_W) if in_req
                    else m.const(PH_IDLE, width=PH_W))

    next_main_phase = (
        np_from_idle if in_idle
        else (np_from_load if in_load
        else (np_from_hist if in_hist
        else (np_from_cum if in_cum
        else (np_from_mask if in_mask
        else (np_from_filter if in_filter
        else (np_from_wait_out if in_wait_out
        else (np_from_drain if in_drain
        else m.const(PH_IDLE, width=PH_W)))))))))

    main_phase.set(next_main_phase)

    # sub_step:  
    # IDLE → 0 (or on in_req start at 0)
    # LOAD: 0..7
    # HIST: 0..7
    # CUMSUM: stays 0 (1 cy)
    # MASK: 0..7
    # FILT_GT: 0..7
    # FILT_EQ: 0..7 or 0
    # WAIT_OUT: 0
    # DRAIN: handled via send_cnt
    # Cleanest:
    #   in_load | in_hist | in_mask | in_filt_gt | in_filt_eq → inc if not at_last else 0
    in_count_phase = in_load | in_hist | in_mask | in_filter
    np_sub = (
        (sub_step_zero if at_beat_last else sub_step_inc)
        if in_count_phase
        else sub_step_zero
    )
    sub_step.set(np_sub)

    # cur_round:
    #   reset to 0 on LOAD start (load_start)
    #   increment at CUMSUM end (in_cum & !is_last_round) — actually transitions through MASK
    #   We increment cur_round when LEAVING CUMSUM phase IF !is_last_round
    incr_round = in_cum & ~is_last_round
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
