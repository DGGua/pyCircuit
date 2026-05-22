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
    from histogram import _zext  # local import to avoid module cycles

    positions: List["Wire"] = []
    running = m.const(0, width=pos_width)
    for f in flags:
        f_ext = _zext(m, f, pos_width)
        nxt = (running.as_unsigned() + f_ext.as_unsigned()).slice(lsb=0, width=pos_width)
        positions.append(nxt)
        running = nxt
    return positions, running


# ═════════════════════════════════════════════════════════════════
# Barrel rotate for output_buf compact write
# ═════════════════════════════════════════════════════════════════

def barrel_rotate(
    m: "CycleAwareCircuit",
    lane_in: Sequence["Wire"],
    rot: "Wire",
) -> List["Wire"]:
    """Cyclic left rotation by ``rot`` lane positions (lane 0 → lane rot).

    For lane_num=128, ``rot`` should be a 7-bit wire. Implemented as a
    flat 128-to-1 mux chain per output lane (~128*128 = 16K LUTs total),
    which is the simplest functional form. arch.md §4.2.2 mentions
    optional log2-stage barrel for area.
    """
    n = len(lane_in)
    width = lane_in[0].width if lane_in else 1
    out: List["Wire"] = []
    for k in range(n):
        # out[k] = lane_in[(k - rot) mod n]
        opts: List["Wire"] = []
        for r in range(n):
            src_idx = (k - r) % n
            opts.append(lane_in[src_idx])
        default = lane_in[k]
        cur = default
        for r, opt in enumerate(opts):
            eq = rot == m.const(r, width=rot.width)
            cur = eq.select(opt, cur)
        out.append(cur)
    return out
