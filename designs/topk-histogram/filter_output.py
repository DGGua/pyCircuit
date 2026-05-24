"""Combinational helpers for filter_compact + output_buf alignment.

State (output_buf SRAMs, send FSM regs, gt_count / eq_remain / total_count)
is allocated INSIDE ``build()`` in `topk_histogram.py` because @function /
plain helpers cannot allocate state (JIT structural check).

References:
    arch.md §4.2.9 filter_compact
    arch.md §4.2.2 output_buf
"""
from __future__ import annotations

from typing import TYPE_CHECKING, List, Sequence

if TYPE_CHECKING:
    from pycircuit import CycleAwareCircuit
    from pycircuit.hw import Wire


def filter_num_chunks(*, lane_num: int, lanes_per_chunk: int) -> int:
    """PH_FILTER cycles per SRAM beat."""
    if lane_num % lanes_per_chunk != 0:
        raise ValueError(
            f"lane_num={lane_num} must be divisible by "
            f"lanes_per_chunk={lanes_per_chunk}"
        )
    return lane_num // lanes_per_chunk


def filter_chunk_lanes(
    m: "CycleAwareCircuit",
    lane_keys: Sequence["Wire"],
    chunk_idx: "Wire",
    *,
    lanes_per_chunk: int,
    num_chunks: int,
) -> List["Wire"]:
    """Return ``lanes_per_chunk`` keys for the selected chunk index."""
    from datapath import mux_phase

    out: List["Wire"] = []
    for i in range(lanes_per_chunk):
        options = [
            lane_keys[c * lanes_per_chunk + i]
            for c in range(num_chunks)
        ]
        out.append(mux_phase(m, chunk_idx, options, default=options[0]))
    return out


# ═════════════════════════════════════════════════════════════════
# Per-lane GT / EQ flags and prefix-sum positions
# ═════════════════════════════════════════════════════════════════

def filter_pass_lanes(
    m: "CycleAwareCircuit",
    lane_keys: Sequence["Wire"],
    kth_key: "Wire",
    *,
    pass_eq: bool,
) -> List["Wire"]:
    """Return a per-lane 1-bit "select this lane" wire.

    GT pass: select if key > kth_key.
    EQ pass: select if key == kth_key.
    Lanes are not masked here — the caller gates by `(pos < remaining)`.
    """
    out: List["Wire"] = []
    for k in lane_keys:
        if pass_eq:
            sel = k == kth_key
        else:
            sel = k.as_unsigned() > kth_key.as_unsigned()
        out.append(sel)
    return out


def prefix_sum_lanes(
    m: "CycleAwareCircuit",
    flags: Sequence["Wire"],
    *,
    pos_width: int,
) -> tuple[List["Wire"], "Wire"]:
    """Inclusive 1-bit prefix sum across ``flags``.

    Returns ``(positions, total)`` where:
      - ``positions[l]`` = sum(flags[0..l]) as ``pos_width``-bit wire
        (so ``positions[l] - 1`` is the lane's 0-based "compact index")
      - ``total`` = sum across all lanes (``pos_width``-bit wire)

    Pure ripple — fine for 128 lanes at modest frequencies. The plan §12
    notes pipelining this if STA fails.
    """
    from histogram_engine import _zext  # local import to avoid module cycles

    positions: List["Wire"] = []
    running = m.const(0, width=pos_width)
    for f in flags:
        f_ext = _zext(m, f, pos_width)
        nxt = (running.as_unsigned() + f_ext.as_unsigned()).slice(lsb=0, width=pos_width)
        positions.append(nxt)
        running = nxt
    return positions, running


def prefix_sum_chunk(
    m: "CycleAwareCircuit",
    flags: Sequence["Wire"],
    running_q: "Wire",
    *,
    pos_width: int,
) -> tuple[List["Wire"], "Wire"]:
    """Inclusive prefix sum over one chunk of ``flags`` starting from ``running_q``.

    Combinational depth is O(len(flags)), not O(LANE_NUM).  RTL calls this once
    per PH_FILTER cycle with at most ``FILTER_LANES_PER_CY`` flags.
    """
    from histogram_engine import _zext  # local import to avoid module cycles

    positions: List["Wire"] = []
    running = running_q
    for f in flags:
        f_ext = _zext(m, f, pos_width)
        nxt = (running + f_ext).slice(lsb=0, width=pos_width)
        positions.append(nxt)
        running = nxt
    return positions, running


def popcount_bits(
    m: "CycleAwareCircuit",
    flags: Sequence["Wire"],
    *,
    out_width: int,
) -> "Wire":
    """Ripple popcount over a small flag vector."""
    from histogram_engine import _zext

    running = m.const(0, width=out_width)
    for f in flags:
        running = (running + _zext(m, f, out_width)).slice(lsb=0, width=out_width)
    return running


def compact_chunk(
    m: "CycleAwareCircuit",
    positions: Sequence["Wire"],
    flags: Sequence["Wire"],
    vals: Sequence["Wire"],
    idxs: Sequence["Wire"],
    run_before: "Wire",
    *,
    lanes_per_chunk: int,
    pos_width: int,
    val_width: int,
) -> tuple[List["Wire"], List["Wire"]]:
    """Compact one FILTER chunk into dense local slots ``[0 .. lanes_per_chunk)``.

    ``run_before`` is the inclusive prefix total *before* this chunk (same width as
    ``positions``).  Global 1-indexed position for local slot ``p`` is
    ``run_before + p + 1``.  Only lanes in the current chunk participate, so
    combinational depth is O(lanes_per_chunk), not O(LANE_NUM).
    """
    from pycircuit import u

    zero = u(val_width, 0)
    compact_vals: List["Wire"] = []
    compact_idxs: List["Wire"] = []
    for p in range(lanes_per_chunk):
        global_pos = (run_before + u(pos_width, p + 1)).slice(lsb=0, width=pos_width)
        v_acc = zero
        i_acc = zero
        for i in range(lanes_per_chunk):
            match = (positions[i] == global_pos) & flags[i]
            v_pick = match.select(vals[i], zero)
            i_pick = match.select(idxs[i], zero)
            v_acc = v_acc | v_pick
            i_acc = i_acc | i_pick
        compact_vals.append(v_acc)
        compact_idxs.append(i_acc)
    return compact_vals, compact_idxs
