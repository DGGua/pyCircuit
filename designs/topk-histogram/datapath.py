"""Small pycircuit datapath helpers shared by the Top-K Histogram RTL.

These live outside ``@module build`` so the JIT only traces the actual
hardware-producing calls inside the helpers (mostly `m.const` / slice / cat /
``.select``). Keep them stateless (no `m.out` here) so they compose freely.

Imports
-------
`fp32_to_sortable_key_hw`/`sortable_key_to_fp32_hw` come from `fp_key`.
"""
from __future__ import annotations

from typing import TYPE_CHECKING, List, Sequence

if TYPE_CHECKING:
    from pycircuit import CycleAwareCircuit
    from pycircuit.hw import Wire


# ═════════════════════════════════════════════════════════════════
# Bus packing helpers (clone of the convention in topk.py:179-184)
# ═════════════════════════════════════════════════════════════════

def unpack_lanes(bus: "Wire", *, lane_w: int, lanes: int) -> List["Wire"]:
    """Slice a packed bus into per-lane wires (lane 0 in LSB)."""
    return [bus.slice(lsb=i * lane_w, width=lane_w) for i in range(lanes)]


def pack_lanes_lsb_first(m: "CycleAwareCircuit", lanes: Sequence["Wire"]) -> "Wire":
    """Pack per-lane wires back into one bus (lane 0 in LSB).

    Requires ``cat`` to be available. We import lazily because this module is
    imported by `topk_histogram_model` which doesn't link pycircuit.
    """
    from pycircuit.hw import cat

    return cat(*reversed(list(lanes)))


# ═════════════════════════════════════════════════════════════════
# Phase mux (clone of `_mux_phase` from topk.py:191-204)
# ═════════════════════════════════════════════════════════════════

def mux_phase(
    m: "CycleAwareCircuit",
    sel: "Wire",
    options: Sequence["Wire"],
    *,
    default: "Wire",
) -> "Wire":
    """Linear mux ``options[sel]`` with a default fallback.

    Implemented as a chain of `.select` so each option has a clear path
    through the netlist. Chain length == len(options); fine up to ~256.
    """
    cur: "Wire" = default
    sel_w = sel.width
    for i, opt in enumerate(options):
        eq = sel == m.const(i, width=sel_w)
        cur = eq.select(opt, cur)
    return cur


# ═════════════════════════════════════════════════════════════════
# Byte select (arch §4.2 / §5.1)
# ═════════════════════════════════════════════════════════════════

def byte_select_lane(
    m: "CycleAwareCircuit",
    key32: "Wire",
    round_idx: "Wire",
) -> "Wire":
    """Pick the radix-byte of ``key32`` for the given round.

    Round 0 → key[31:24] (MSB), round 3 → key[7:0] (LSB). Matches the
    Python-side `_byte_of_key` in `topk_histogram_model.py`.
    """
    assert key32.width == 32, f"byte_select_lane expects 32-bit key (got {key32.width})"
    byte_msb = key32.slice(lsb=24, width=8)
    byte_b   = key32.slice(lsb=16, width=8)
    byte_c   = key32.slice(lsb=8,  width=8)
    byte_lsb = key32.slice(lsb=0,  width=8)
    return mux_phase(
        m, round_idx,
        [byte_msb, byte_b, byte_c, byte_lsb],
        default=byte_msb,
    )


# ═════════════════════════════════════════════════════════════════
# Mask row update (arch §4.2.7)
# ═════════════════════════════════════════════════════════════════

def mask_row_update(
    m: "CycleAwareCircuit",
    mask_reg_q: "Wire",
    *,
    row_idx: int,
    lane_keep: Sequence["Wire"],
    total_rows: int,
    lane_num: int,
) -> "Wire":
    """Replace one ``lane_num``-bit row of ``mask_reg_q`` with ``lane_keep`` bits.

    The mask register is laid out as ``[row total_rows-1 : row 0]`` with row 0
    in the LSB block; within each row, lane 0 is the LSB. ``lane_keep`` should
    contain ``lane_num`` 1-bit wires (1 = keep, 0 = drop).

    Returns the full ``total_rows * lane_num``-bit next-state wire suitable
    for `mask_reg.set(...)`.
    """
    from pycircuit.hw import cat

    if len(lane_keep) != lane_num:
        raise ValueError(
            f"mask_row_update: expected {lane_num} lane bits, got {len(lane_keep)}"
        )
    new_row = cat(*reversed(list(lane_keep)))    # lane 0 in LSB
    # Combine: [high rows kept] :: [new row] :: [low rows kept]
    pieces: list["Wire"] = []
    # cat() takes MSB-first arguments; build from high to low.
    for r in range(total_rows - 1, -1, -1):
        if r == row_idx:
            pieces.append(new_row)
        else:
            piece = mask_reg_q.slice(lsb=r * lane_num, width=lane_num)
            pieces.append(piece)
    return cat(*pieces)


# ═════════════════════════════════════════════════════════════════
# KTH compose (arch §4.2.8)
# ═════════════════════════════════════════════════════════════════

def kth_compose(target_bin_lat: Sequence["Wire"]) -> "Wire":
    """Concatenate four 8-bit ``target_bin_lat[0..3]`` into a 32-bit kth_key.

    ``target_bin_lat[0]`` is the MSB byte (round 0), ``target_bin_lat[3]`` is
    the LSB byte (round 3) — same byte order as `_byte_of_key`.
    """
    from pycircuit.hw import cat

    if len(target_bin_lat) != 4:
        raise ValueError(f"kth_compose expects 4 bytes, got {len(target_bin_lat)}")
    return cat(target_bin_lat[0], target_bin_lat[1], target_bin_lat[2], target_bin_lat[3])
