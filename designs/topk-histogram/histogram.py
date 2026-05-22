"""Combinational helpers for the histogram + cumsum + priority encoder path.

State (`hist_b{...}` accumulators, pipeline registers) is allocated INSIDE
``build()`` in `topk_histogram.py` so the JIT structural-metrics check
(`state_call_count > 0` in a plain helper triggers `JitError`) never fires.
Everything in this module is pure combinational wire glue.

References:
    arch.md §4.2.5 histogram_engine
    arch.md §4.2.6 cumsum_threshold
"""
from __future__ import annotations

from typing import TYPE_CHECKING, List, Sequence

if TYPE_CHECKING:
    from pycircuit import CycleAwareCircuit
    from pycircuit.hw import Wire


# ═════════════════════════════════════════════════════════════════
# Popcount tree (carry-save style, recursive halving)
# ═════════════════════════════════════════════════════════════════

def popcount_tree(
    m: "CycleAwareCircuit",
    bits: Sequence["Wire"],
    *,
    out_width: int,
) -> "Wire":
    """Binary-tree popcount of a list of 1-bit wires → ``out_width``-bit Wire.

    Resource: ~len(bits) full adders worth of LUTs (log2 depth). For 128
    inputs that is ~127 FAs / ~127 LUTs (7-level tree).
    """
    if not bits:
        return m.const(0, width=out_width)
    cur: List["Wire"] = list(bits)
    for w in cur:
        assert w.width == 1, f"popcount_tree expects 1-bit wires (got {w.width})"
    width = 1
    while len(cur) > 1:
        next_cur: List["Wire"] = []
        # pair up adjacent items; promote both to next width
        for i in range(0, len(cur), 2):
            a = cur[i]
            if i + 1 < len(cur):
                b = cur[i + 1]
            else:
                b = m.const(0, width=width)
            # zero-extend to width+1 before adding so the carry fits.
            a_ext = _zext(m, a, width + 1)
            b_ext = _zext(m, b, width + 1)
            s = (a_ext.as_unsigned() + b_ext.as_unsigned()).slice(lsb=0, width=width + 1)
            next_cur.append(s)
        cur = next_cur
        width += 1
    # Final size adjust to out_width
    only = cur[0]
    if only.width == out_width:
        return only
    if only.width < out_width:
        return _zext(m, only, out_width)
    return only.slice(lsb=0, width=out_width)


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
# Per-bin hit + count for one cycle of histogram accumulation
# ═════════════════════════════════════════════════════════════════

def hist_cycle_counts(
    m: "CycleAwareCircuit",
    lane_bytes: Sequence["Wire"],
    lane_valid: Sequence["Wire"],
    *,
    num_bins: int,
    count_width: int,
) -> List["Wire"]:
    """Per bin, count how many of the lanes hit this bin (this cycle).

    Returns a list of ``num_bins`` wires, each ``count_width`` bits wide.
    For lane_num=128, count_width=8 is enough (max 128 ≤ 255).
    """
    assert len(lane_bytes) == len(lane_valid)
    lane_num = len(lane_bytes)
    for w in lane_bytes:
        assert w.width == 8, f"lane_bytes must be 8 bits (got {w.width})"
    for w in lane_valid:
        assert w.width == 1, f"lane_valid must be 1 bit (got {w.width})"

    out: List["Wire"] = []
    for b in range(num_bins):
        b_const = m.const(b, width=8)
        hits: List["Wire"] = []
        for l in range(lane_num):
            eq = lane_bytes[l] == b_const
            hit = eq & lane_valid[l]
            hits.append(hit)
        out.append(popcount_tree(m, hits, out_width=count_width))
    return out


# ═════════════════════════════════════════════════════════════════
# Cumsum + threshold + priority encoder (combinational)
# ═════════════════════════════════════════════════════════════════

def cumsum_threshold(
    m: "CycleAwareCircuit",
    hist: Sequence["Wire"],
    bottomK: "Wire",
    *,
    bin_width: int,
    hist_width: int,
) -> tuple["Wire", "Wire"]:
    """Return ``(target_bin, prev_cum)`` from a 256-entry histogram.

    ``target_bin`` is the smallest bin index whose inclusive cumulative count
    is ≥ ``bottomK``. ``prev_cum`` is the cumulative count just BELOW that bin
    (i.e. the count of strictly-smaller elements outside the target bin).

    Combinational: O(num_bins) ripple add for the cumsum + O(num_bins) priority
    encoder. For 256 bins this is on the order of a few hundred LUTs deep,
    which is the slowest path in v1 (the plan §12 notes register-pipelining
    this stage if STA fails).
    """
    num_bins = len(hist)
    # Ripple cumulative sum (inclusive)
    cumsum: List["Wire"] = []
    running = m.const(0, width=hist_width + 1)
    for b in range(num_bins):
        running_next = (running.as_unsigned() + hist[b].as_unsigned()).slice(
            lsb=0, width=hist_width + 1
        )
        cumsum.append(running_next)
        running = running_next

    # Priority encode: smallest b such that cumsum[b] >= bottomK
    bottomK_ext = _zext(m, bottomK, hist_width + 1)
    ge_flags: List["Wire"] = [
        cumsum[b].as_unsigned() >= bottomK_ext.as_unsigned() for b in range(num_bins)
    ]
    # target_bin = min b s.t. ge_flags[b] == 1; if none, return num_bins-1 (fallback)
    fallback = m.const(num_bins - 1, width=bin_width)
    target_bin: "Wire" = fallback
    for b in range(num_bins - 1, -1, -1):
        b_const = m.const(b, width=bin_width)
        target_bin = ge_flags[b].select(b_const, target_bin)

    # prev_cum = cumsum[target_bin - 1] if target_bin > 0 else 0
    # Implemented as another priority chain: prev_cum = cumsum[target_bin] - hist[target_bin]
    # (avoids needing the b-1 indexed lookup).
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
    """Linear mux ``options[idx]`` — same pattern as datapath.mux_phase but
    intended for wider option lists (256). Output width is ``out_width``.
    """
    cur = default
    for i, opt in enumerate(options):
        eq = idx == m.const(i, width=idx.width)
        cur = eq.select(opt, cur)
    # Normalize output width
    if cur.width == out_width:
        return cur
    if cur.width < out_width:
        return _zext(m, cur, out_width)
    return cur.slice(lsb=0, width=out_width)
