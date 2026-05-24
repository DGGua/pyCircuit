"""``histogram_engine`` — 4-round radix-select core (hist + cumsum + mask).

Logical block for the Top-K Histogram accelerator.  Combinational helpers
live here; registers (``hist_b*``, ``mask_reg``, ``target_bin_lat``,
``bottomK``) stay in ``topk_histogram.build()`` because pyCircuit JIT
rejects ``m.out`` inside plain helpers.

Submodules
----------
hist_accum
    Per-cycle 256-bin population counts from sortable keys + mask row.
cumsum
    Pipelined inclusive prefix-sum + threshold (``cumsum_chunk_step``); reference
    single-cycle ``cumsum_threshold`` kept for golden checks.
mask
    Row select, lane refine vs ``target_bin``, full-register row replace.
"""
from __future__ import annotations

from typing import TYPE_CHECKING, List, Sequence, Tuple

from datapath import mux_phase

if TYPE_CHECKING:
    from pycircuit import CycleAwareCircuit
    from pycircuit.hw import Wire


# ═════════════════════════════════════════════════════════════════
# Shared utilities
# ═════════════════════════════════════════════════════════════════

def _zext(m: "CycleAwareCircuit", w: "Wire", target_width: int) -> "Wire":
    """Zero-extend ``w`` to ``target_width`` bits (no-op if already wide enough)."""
    from pycircuit.hw import cat

    if w.width == target_width:
        return w
    if w.width > target_width:
        return w.slice(lsb=0, width=target_width)
    pad = m.const(0, width=target_width - w.width)
    return cat(pad, w)


# ═════════════════════════════════════════════════════════════════
# Submodule: hist_accum
# ═════════════════════════════════════════════════════════════════

def popcount_tree(
    m: "CycleAwareCircuit",
    bits: Sequence["Wire"],
    *,
    out_width: int,
) -> "Wire":
    """Binary-tree popcount of a list of 1-bit wires → ``out_width``-bit Wire."""
    if not bits:
        return m.const(0, width=out_width)
    cur: List["Wire"] = list(bits)
    for w in cur:
        assert w.width == 1, f"popcount_tree expects 1-bit wires (got {w.width})"
    width = 1
    while len(cur) > 1:
        next_cur: List["Wire"] = []
        for i in range(0, len(cur), 2):
            a = cur[i]
            b = cur[i + 1] if i + 1 < len(cur) else m.const(0, width=width)
            a_ext = _zext(m, a, width + 1)
            b_ext = _zext(m, b, width + 1)
            s = (a_ext.as_unsigned() + b_ext.as_unsigned()).slice(lsb=0, width=width + 1)
            next_cur.append(s)
        cur = next_cur
        width += 1
    only = cur[0]
    if only.width == out_width:
        return only
    if only.width < out_width:
        return _zext(m, only, out_width)
    return only.slice(lsb=0, width=out_width)


def _radix_byte_lane(
    m: "CycleAwareCircuit",
    key32: "Wire",
    round_idx: "Wire",
) -> "Wire":
    """Pick the radix byte of ``key32`` for ``round_idx`` (MSB-first, rounds 0..3)."""
    assert key32.width == 32, f"_radix_byte_lane expects 32-bit key (got {key32.width})"
    byte_msb = key32.slice(lsb=24, width=8)
    byte_b = key32.slice(lsb=16, width=8)
    byte_c = key32.slice(lsb=8, width=8)
    byte_lsb = key32.slice(lsb=0, width=8)
    return mux_phase(
        m, round_idx,
        [byte_msb, byte_b, byte_c, byte_lsb],
        default=byte_msb,
    )


def hist_cycle_counts(
    m: "CycleAwareCircuit",
    lane_keys: Sequence["Wire"],
    lane_valid: Sequence["Wire"],
    round_idx: "Wire",
    *,
    num_bins: int,
    count_width: int,
) -> List["Wire"]:
    """Per bin, count masked lanes whose radix byte hits this bin (this cycle)."""
    assert len(lane_keys) == len(lane_valid)
    lane_num = len(lane_keys)
    for w in lane_keys:
        assert w.width == 32, f"lane_keys must be 32 bits (got {w.width})"
    for w in lane_valid:
        assert w.width == 1, f"lane_valid must be 1 bit (got {w.width})"

    out: List["Wire"] = []
    for b in range(num_bins):
        b_const = m.const(b, width=8)
        hits: List["Wire"] = []
        for l in range(lane_num):
            lane_byte = _radix_byte_lane(m, lane_keys[l], round_idx)
            hit = (lane_byte == b_const) & lane_valid[l]
            hits.append(hit)
        out.append(popcount_tree(m, hits, out_width=count_width))
    return out


# ═════════════════════════════════════════════════════════════════
# Submodule: cumsum
# ═════════════════════════════════════════════════════════════════

def cumsum_num_chunks(*, num_bins: int, bins_per_chunk: int) -> int:
    """PH_CUMSUM cycles per radix round."""
    if num_bins % bins_per_chunk != 0:
        raise ValueError(
            f"num_bins={num_bins} must be divisible by bins_per_chunk={bins_per_chunk}"
        )
    return num_bins // bins_per_chunk


def _hist_bin_in_chunk(
    m: "CycleAwareCircuit",
    hist: Sequence["Wire"],
    chunk_idx: "Wire",
    offset_in_chunk: int,
    *,
    bins_per_chunk: int,
    num_chunks: int,
) -> "Wire":
    """Mux ``hist[chunk_idx * bins_per_chunk + offset_in_chunk]`` (depth ≈ num_chunks)."""
    options = [
        hist[c * bins_per_chunk + offset_in_chunk]
        for c in range(num_chunks)
    ]
    return mux_phase(
        m, chunk_idx, options, default=options[0],
    )


def cumsum_chunk_step(
    m: "CycleAwareCircuit",
    hist: Sequence["Wire"],
    bottomK: "Wire",
    cum_running_q: "Wire",
    chunk_idx: "Wire",
    *,
    bins_per_chunk: int,
    num_chunks: int,
    bin_width: int,
    hist_width: int,
    target_found_q: "Wire",
    target_bin_q: "Wire",
    prev_cum_q: "Wire",
    hist_at_target_q: "Wire",
) -> tuple["Wire", "Wire", "Wire", "Wire", "Wire"]:
    """One PH_CUMSUM cycle: ripple at most ``bins_per_chunk`` bins + sticky first-hit.

    Reuses the same chunk datapath each cycle; ``chunk_idx`` selects which slice
    of ``hist`` participates.  Combinational depth is O(bins_per_chunk + num_chunks),
    not O(num_bins).

    Returns ``(running_out, target_found_out, target_bin_out, prev_cum_out,
    hist_at_target_out)``.
    """
    running = cum_running_q
    bottomK_ext = _zext(m, bottomK, hist_width + 1)
    tgt_found = target_found_q
    tgt_bin = target_bin_q
    prev_cum = prev_cum_q
    hist_at = hist_at_target_q

    for i in range(bins_per_chunk):
        hist_b = _hist_bin_in_chunk(
            m, hist, chunk_idx, i,
            bins_per_chunk=bins_per_chunk, num_chunks=num_chunks,
        )
        running_before = running
        running_next = (
            running.as_unsigned()
            + _zext(m, hist_b, hist_width + 1).as_unsigned()
        ).slice(lsb=0, width=hist_width + 1)

        ge = running_next.as_unsigned() >= bottomK_ext.as_unsigned()
        hit = ge & ~tgt_found

        abs_bin = (
            chunk_idx.as_unsigned()
            * m.const(bins_per_chunk, width=bin_width).as_unsigned()
            + m.const(i, width=bin_width).as_unsigned()
        ).slice(lsb=0, width=bin_width)

        tgt_bin = hit.select(abs_bin, tgt_bin)
        prev_cum = hit.select(
            running_before.slice(lsb=0, width=hist_width), prev_cum,
        )
        hist_at = hit.select(hist_b, hist_at)
        tgt_found = tgt_found | hit
        running = running_next

    return running, tgt_found, tgt_bin, prev_cum, hist_at


def cumsum_threshold(
    m: "CycleAwareCircuit",
    hist: Sequence["Wire"],
    bottomK: "Wire",
    *,
    bin_width: int,
    hist_width: int,
) -> tuple["Wire", "Wire"]:
    """Return ``(target_bin, prev_cum)`` from a 256-entry histogram.

    Reference / golden helper (single-cycle, full depth).  RTL uses
    :func:`cumsum_chunk_step` over multiple PH_CUMSUM cycles instead.
    """
    num_bins = len(hist)
    cumsum: List["Wire"] = []
    running = m.const(0, width=hist_width + 1)
    for b in range(num_bins):
        running_next = (running.as_unsigned() + hist[b].as_unsigned()).slice(
            lsb=0, width=hist_width + 1
        )
        cumsum.append(running_next)
        running = running_next

    bottomK_ext = _zext(m, bottomK, hist_width + 1)
    ge_flags: List["Wire"] = [
        cumsum[b].as_unsigned() >= bottomK_ext.as_unsigned() for b in range(num_bins)
    ]
    fallback = m.const(num_bins - 1, width=bin_width)
    target_bin: "Wire" = fallback
    for b in range(num_bins - 1, -1, -1):
        b_const = m.const(b, width=bin_width)
        target_bin = ge_flags[b].select(b_const, target_bin)

    hist_at_tgt = _mux_index(m, target_bin, list(hist), default=hist[0], out_width=hist_width)
    cum_at_tgt = _mux_index(
        m, target_bin, list(cumsum), default=cumsum[0], out_width=hist_width + 1,
    )
    prev_cum_full = (cum_at_tgt.as_unsigned() - _zext(m, hist_at_tgt, hist_width + 1).as_unsigned()
                     ).slice(lsb=0, width=hist_width + 1)
    prev_cum = prev_cum_full.slice(lsb=0, width=hist_width)

    return target_bin, prev_cum


def _mux_index(
    m: "CycleAwareCircuit",
    idx: "Wire",
    options: List["Wire"],
    *,
    default: "Wire",
    out_width: int,
) -> "Wire":
    """Linear mux ``options[idx]`` for wide option lists (e.g. 256 bins)."""
    cur = default
    for i, opt in enumerate(options):
        eq = idx == m.const(i, width=idx.width)
        cur = eq.select(opt, cur)
    if cur.width == out_width:
        return cur
    if cur.width < out_width:
        return _zext(m, cur, out_width)
    return cur.slice(lsb=0, width=out_width)


# ═════════════════════════════════════════════════════════════════
# Submodule: mask
# ═════════════════════════════════════════════════════════════════

def mask_row_lanes_for_beat(
    m: "CycleAwareCircuit",
    mask_reg_q: "Wire",
    beat_sel: "Wire",
    *,
    burst_len: int,
    lane_num: int,
) -> Tuple["Wire", List["Wire"]]:
    """Mux the 128-lane mask row for the SRAM beat on ``sram_rdata`` (``sub_step_lo``)."""
    mask_row_options = [
        mask_reg_q.slice(lsb=row * lane_num, width=lane_num)
        for row in range(burst_len)
    ]
    mask_row = mux_phase(m, beat_sel, mask_row_options, default=mask_row_options[0])
    mask_row_lanes = [mask_row.slice(lsb=l, width=1) for l in range(lane_num)]
    return mask_row, mask_row_lanes


def target_bin_for_mask(
    m: "CycleAwareCircuit",
    target_bin_lat: Sequence["Wire"],
    cur_round: "Wire",
) -> "Wire":
    """``target_bin`` for the round just completed (MASK runs after CUMSUM bump)."""
    return mux_phase(
        m, cur_round,
        [target_bin_lat[0], target_bin_lat[0], target_bin_lat[1], target_bin_lat[2]],
        default=target_bin_lat[0],
    )


def prior_round_idx(m: "CycleAwareCircuit", cur_round: "Wire") -> "Wire":
    """Round index for mask refine (= ``cur_round - 1``)."""
    return (cur_round.as_unsigned() - m.const(1, width=2).as_unsigned()
            ).slice(lsb=0, width=2)


def mask_refine_lane_bits(
    m: "CycleAwareCircuit",
    lane_keys: Sequence["Wire"],
    round_idx: "Wire",
    target_bin: "Wire",
    mask_row_lanes: Sequence["Wire"],
) -> List["Wire"]:
    """Refine one mask row: keep lane iff mask bit set and radix byte == ``target_bin``."""
    assert len(lane_keys) == len(mask_row_lanes)
    out: List["Wire"] = []
    for l in range(len(lane_keys)):
        lane_byte = _radix_byte_lane(m, lane_keys[l], round_idx)
        out.append(mask_row_lanes[l] & (lane_byte == target_bin))
    return out


def mask_row_update(
    m: "CycleAwareCircuit",
    mask_reg_q: "Wire",
    *,
    row_idx: int,
    lane_keep: Sequence["Wire"],
    total_rows: int,
    lane_num: int,
) -> "Wire":
    """Replace one ``lane_num``-bit row of ``mask_reg_q`` with ``lane_keep`` bits."""
    from pycircuit.hw import cat

    if len(lane_keep) != lane_num:
        raise ValueError(
            f"mask_row_update: expected {lane_num} lane bits, got {len(lane_keep)}"
        )
    new_row = cat(*reversed(list(lane_keep)))
    pieces: list["Wire"] = []
    for r in range(total_rows - 1, -1, -1):
        if r == row_idx:
            pieces.append(new_row)
        else:
            pieces.append(mask_reg_q.slice(lsb=r * lane_num, width=lane_num))
    return cat(*pieces)


def mask_row_replace(
    m: "CycleAwareCircuit",
    mask_reg_q: "Wire",
    new_lane_bits: Sequence["Wire"],
    beat_sel: "Wire",
    *,
    burst_len: int,
    lane_num: int,
) -> "Wire":
    """Full ``mask_reg`` with one row replaced (beat = ``beat_sel``). Phase mux stays in ``build()``."""
    mask_next_per_row = [
        mask_row_update(
            m, mask_reg_q,
            row_idx=r,
            lane_keep=new_lane_bits,
            total_rows=burst_len,
            lane_num=lane_num,
        )
        for r in range(burst_len)
    ]
    return mux_phase(m, beat_sel, mask_next_per_row, default=mask_reg_q)
