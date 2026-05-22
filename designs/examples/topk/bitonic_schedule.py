"""Batcher Bitonic sort/merge schedule generators (pure Python).

Each schedule is a flat list of layers. Each layer is

    (stride: int, pairs: list[tuple[lo, hi, dir]])

where ``dir == 1`` (DESC) means ``cmp_swap`` puts MAX at the lower index, and
``dir == 0`` (ASC) means MAX at the higher index.

A whole layer is a *combinational* row of ``cmp_swap`` cells. All pairs in the
same layer have **disjoint indices** so they execute in parallel; the per-pair
``dir`` is consumed by each ``cmp_swap``'s ``dir`` port.

Three schedule families are exposed:

  - ``gen_sort_schedule_desc(N)``         — Batcher Bitonic sort of N (= 2^p)
                                            elements producing descending output.
                                            ``log2(N)·(log2(N)+1)/2`` layers, each
                                            with N/2 pairs.
  - ``gen_full_merge_2p_desc(P)``         — 2P → 2P full descending bitonic merge
                                            (used by streaming carry path).
                                            ``log2(P) + 1`` layers, each with P pairs.
  - ``gen_merge_half_schedule_2p(P)``     — Half-layer split of the full merge:
                                            each merge layer is sliced into a
                                            **lo-half** (P/2 pairs with lo<P) and
                                            a **hi-half** (P/2 pairs with lo≥P).
                                            Used by the unified shared-128-cas
                                            engine where every cycle drives 128
                                            cas cells (= P/2 for P=256). Total:
                                            ``2 · (log2(P) + 1)`` half-layers.

A pure-Python software model is provided (``apply_schedule``). Self-tests live
under ``selftest/`` (see ``selftest/test_all.py``).
"""
from __future__ import annotations

import math
from typing import List, Tuple

# (lo, hi, dir): per-pair direction, lo < hi (always)
Pair = Tuple[int, int, int]
Layer = Tuple[int, List[Pair]]
Schedule = List[Layer]

DESC = 1
ASC = 0


def _is_pow2(n: int) -> bool:
    return n > 0 and (n & (n - 1)) == 0


# ═════════════════════════════════════════════════════════════════
# Bitonic sort (Batcher recursive, descending output)
# ═════════════════════════════════════════════════════════════════

def gen_sort_schedule_desc(N: int) -> Schedule:
    """Generate Batcher Bitonic sort schedule producing **descending** order.

    The schedule has ``log2(N)·(log2(N)+1)/2`` layers, each with ``N/2`` pairs.

    Within layer (k, j), each pair lives in a 2^k-block. Even-indexed blocks
    contribute DESC pairs, odd-indexed contribute ASC; a single layer thus has
    mixed-direction pairs (carried in the per-pair dir field).
    """
    if not _is_pow2(N):
        raise ValueError(f"N must be a power of 2 (got {N})")
    p = int(math.log2(N))
    sched: Schedule = []
    for k in range(1, p + 1):
        block_w = 1 << k
        for j in range(k - 1, -1, -1):
            stride = 1 << j
            pairs: list[Pair] = []
            for i in range(N):
                partner = i ^ stride
                if i < partner:
                    block = i // block_w
                    direction = DESC if (block & 1) == 0 else ASC
                    pairs.append((i, partner, direction))
            assert len(pairs) == N // 2, f"layer (k={k}, j={j}) has {len(pairs)} pairs, expected {N//2}"
            sched.append((stride, pairs))
    return sched


# ═════════════════════════════════════════════════════════════════
# Full 2P → 2P bitonic merge (for streaming Stage B carry path)
# ═════════════════════════════════════════════════════════════════

def gen_full_merge_2p_desc(P: int) -> Schedule:
    """Schedule for full 2P → 2P bitonic merge cell, descending output.

    Same wiring convention as ``gen_topk_merge_schedule``: low half holds
    ``A[i]`` (desc), high half holds ``B[P-1-j]`` (so the concat is valley
    bitonic). All 2P lanes are kept in the output (top P plus carry P).
    """
    if not _is_pow2(P):
        raise ValueError(f"P must be a power of 2 (got {P})")
    sched: Schedule = []
    p = int(math.log2(P)) + 1
    width = 2 * P
    for level in range(p):
        stride = (width >> 1) >> level
        pairs = [(i, i + stride, DESC) for i in range(width) if (i & stride) == 0]
        sched.append((stride, pairs))
    return sched


# ═════════════════════════════════════════════════════════════════
# Half-layer split of the full 2P merge (for the shared 128-cas bank)
# ═════════════════════════════════════════════════════════════════

def gen_merge_half_schedule_2p(P: int) -> Schedule:
    """Half-layer split of ``gen_full_merge_2p_desc(P)``.

    The full merge has ``log2(P) + 1`` layers, each with ``P`` disjoint
    pairs. On hardware with a 128-cas bank (= P/2 cells when P=256) we
    can only fire P/2 pairs per cycle, so each merge layer is split into
    two cycles. The split is **equal-by-pair-index**: half 0 runs the
    first P/2 pairs of the layer, half 1 runs the last P/2 pairs.

    Pairs within a single full layer are disjoint regardless of how we
    partition them, so any equal split is correctness-preserving. The
    "first P/2 vs last P/2" split keeps both halves the same size for
    every layer (the {lo<P, lo>=P} split would have given an unequal
    P / 0 split on layer 0 where all pairs have lo<P).

    Returns ``2 · (log2(P) + 1)`` half-layers, each with P/2 pairs.
    """
    if not _is_pow2(P):
        raise ValueError(f"P must be a power of 2 (got {P})")
    full = gen_full_merge_2p_desc(P)
    half: Schedule = []
    half_p = max(1, P // 2)
    for stride, pairs in full:
        first_half  = pairs[:half_p]
        second_half = pairs[half_p:]
        if P > 1:
            assert len(first_half) == half_p and len(second_half) == half_p, (
                f"merge half-split mismatch: stride={stride}, "
                f"first={len(first_half)}, second={len(second_half)}, P={P}"
            )
        half.append((stride, first_half))
        half.append((stride, second_half))
    return half


def apply_half_merge_2p(A: list, B: list) -> list:
    """Software reference: full merge using the half-layer schedule.

    Equivalent to ``full_merge_2p_apply(A, B)`` modulo schedule walking
    order. Used by selftests to verify that the half-split yields a
    sorted 2P-element output.
    """
    P = len(A)
    assert len(B) == P
    laned = list(A) + list(reversed(B))
    sched = gen_merge_half_schedule_2p(P)
    return apply_schedule(laned, sched)


# ═════════════════════════════════════════════════════════════════
# Software model: apply a schedule to a Python list (used by tests)
# ═════════════════════════════════════════════════════════════════

def apply_layer(arr: list, layer: Layer) -> list:
    """Apply one schedule layer to ``arr``, returning a new list."""
    out = list(arr)
    _, pairs = layer
    for lo, hi, direction in pairs:
        a = out[lo]
        b = out[hi]
        if direction == DESC:
            new_lo, new_hi = (a, b) if a >= b else (b, a)
        else:
            new_lo, new_hi = (a, b) if a <= b else (b, a)
        out[lo] = new_lo
        out[hi] = new_hi
    return out


def apply_schedule(arr: list, sched: Schedule) -> list:
    out = list(arr)
    for layer in sched:
        out = apply_layer(out, layer)
    return out


def full_merge_2p_apply(A: list, B: list) -> list:
    """Software reference for bitonic_merge_2p_full: full 2P desc-sorted result."""
    P = len(A)
    assert len(B) == P
    laned = list(A) + list(reversed(B))
    sched = gen_full_merge_2p_desc(P)
    return apply_schedule(laned, sched)
