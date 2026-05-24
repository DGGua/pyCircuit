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
