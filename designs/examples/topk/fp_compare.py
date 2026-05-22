"""Unified ``less-than`` comparator with runtime ``fmt_sel``.

Supports five formats at runtime via a single 3-bit ``fmt_sel`` input:

    bf16, fp16, fp32, fp8_e4m3, fp4_e2m1

Design:

  - Hardware takes 32-bit value slots ``a32``, ``b32`` and a 3-bit ``fmt_sel``
    runtime input. Five monotone-key transforms (one per fmt) are built in
    parallel, each followed by an unsigned-compare; the five lt bits are
    8:1-muxed on ``fmt_sel`` (positions 5..7 reserved, sel them returns lt_fp32).
  - Two transforms inside each fmt path:
        1. NaN-fold:   NaN bit patterns are replaced by the fmt's
                       ``neg_inf_bits`` (= NEG_MAX_FINITE for fmts without ±∞)
                       so they always lose a compare. Skipped entirely for
                       fmts with ``has_nan=False`` (fp4_e2m1).
        2. Sign-magnitude → monotone unsigned: standard IEEE 754 trick so that
                       unsigned compare matches float compare for all finite,
                       ±0, subnormal, and ±inf values.

Python reference (golden model, testbenches): see ``tool.py``.

Hardware: ``fp_lt(a32, b32, fmt_sel)`` below.
"""
from __future__ import annotations

import math
from typing import Tuple

from pycircuit.hw import Wire

from topk_config import (
    FpFormat,
    FP_FORMATS,
    FMT_BF16, FMT_FP16, FMT_FP32, FMT_FP8_E4M3, FMT_FP4_E2M1,
    FMT_SEL_W,
    VAL_W,
)


# ═════════════════════════════════════════════════════════════════
# Hardware comparator (pyCircuit Wire-level)
# ═════════════════════════════════════════════════════════════════

def _fp_to_key_for_fmt(a_slice: Wire, fmt: FpFormat) -> Tuple[Wire, int]:
    """Build NaN-fold + sign-magnitude → monotone unsigned key for ``fmt``.

    ``a_slice`` must already be the correct native width for ``fmt``. Returns
    ``(key_wire, depth)`` where depth is a comb-depth estimate (used by the
    test harness; not consumed by the synth flow).

    Per-format behavior:
      - fmt.has_nan=False (fp4_e2m1): no NaN folding, just sign-magnitude →
        monotone. Every encoding is finite.
      - fmt.has_inf=False but fmt.has_nan=True (fp8_e4m3): NaN test is
        ``exp=all-1 & man=all-1`` (single NaN encoding). Folded to
        ``neg_inf_bits`` (= NEG_MAX_FINITE bit pattern).
      - fmt.has_inf=True  (bf16/fp16/fp32): NaN test is
        ``exp=all-1 & man != 0``. Folded to true ±∞ negative encoding.
    """
    W = fmt.width
    m = a_slice.m

    sign_mask     = m.const(1 << fmt.sign_bit, width=W)
    all_ones_mask = m.const((1 << W) - 1, width=W)
    neg_inf_bits  = m.const(fmt.neg_inf_bits, width=W)

    fold_depth = 0

    if fmt.has_nan:
        exp_field = a_slice.slice(lsb=fmt.exp_lsb, width=fmt.exp_w)
        zero_man  = m.const(0, width=fmt.man_w)
        exp_all_ones = m.const(fmt.exp_all_ones, width=fmt.exp_w)
        exp_is_max   = exp_field == exp_all_ones
        if fmt.has_inf:
            # IEEE-like: NaN iff exp_all_ones AND man != 0
            man_field = a_slice.slice(lsb=0, width=fmt.man_w)
            man_nonzero = man_field != zero_man
            is_nan = exp_is_max & man_nonzero
        else:
            # fp8_e4m3 single-NaN: NaN iff exp_all_ones AND man=man_all_ones
            man_field = a_slice.slice(lsb=0, width=fmt.man_w)
            man_all_ones = m.const(fmt.man_all_ones, width=fmt.man_w)
            man_is_max = man_field == man_all_ones
            is_nan = exp_is_max & man_is_max
        nan_depth = max(1, int(math.ceil(math.log2(max(fmt.exp_w, fmt.man_w))))) + 2
        folded = is_nan.select(neg_inf_bits, a_slice)
        fold_depth = nan_depth + 2
    else:
        folded = a_slice

    folded_sign = folded.slice(lsb=fmt.sign_bit, width=1)
    flip_sign_only = folded ^ sign_mask
    flip_all       = folded ^ all_ones_mask
    key = folded_sign.select(flip_all, flip_sign_only)
    sm_depth = 1 + 2

    return key, fold_depth + sm_depth


def fp_lt(a32: Wire, b32: Wire, fmt_sel: Wire) -> Tuple[Wire, int]:
    """Hardware ``a < b`` with runtime ``fmt_sel``.

    Args:
        a32, b32 : ``VAL_W``-bit value Wires. Narrow fmts use the low bits.
        fmt_sel  : 3-bit Wire. 0=bf16, 1=fp16, 2=fp32, 3=fp8_e4m3, 4=fp4_e2m1;
                   5..7 reserved (return lt_fp32).

    Returns ``(lt_bit, depth)``. ``lt_bit`` is 1 iff
    ``float_of(a, fmt_sel) < float_of(b, fmt_sel)`` in the NaN-folded ordering.
    """
    if a32.width != VAL_W or b32.width != VAL_W:
        raise ValueError(
            f"fp_lt: value width must be {VAL_W} (got a={a32.width}, b={b32.width})"
        )
    if fmt_sel.width != FMT_SEL_W:
        raise ValueError(f"fp_lt: fmt_sel width must be {FMT_SEL_W} (got {fmt_sel.width})")

    m = a32.m

    # Per-fmt slices (low bits of the unified VAL_W lane).
    a_bf = a32.slice(lsb=0, width=16)
    b_bf = b32.slice(lsb=0, width=16)
    a_h  = a32.slice(lsb=0, width=16)
    b_h  = b32.slice(lsb=0, width=16)
    a_s  = a32.slice(lsb=0, width=32)
    b_s  = b32.slice(lsb=0, width=32)
    a_8  = a32.slice(lsb=0, width=8)
    b_8  = b32.slice(lsb=0, width=8)
    a_4  = a32.slice(lsb=0, width=4)
    b_4  = b32.slice(lsb=0, width=4)

    key_a_bf, d_bf = _fp_to_key_for_fmt(a_bf, FP_FORMATS["bf16"])
    key_b_bf, _    = _fp_to_key_for_fmt(b_bf, FP_FORMATS["bf16"])
    lt_bf = key_a_bf.as_unsigned() < key_b_bf.as_unsigned()

    key_a_h, d_h = _fp_to_key_for_fmt(a_h, FP_FORMATS["fp16"])
    key_b_h, _   = _fp_to_key_for_fmt(b_h, FP_FORMATS["fp16"])
    lt_h = key_a_h.as_unsigned() < key_b_h.as_unsigned()

    key_a_s, d_s = _fp_to_key_for_fmt(a_s, FP_FORMATS["fp32"])
    key_b_s, _   = _fp_to_key_for_fmt(b_s, FP_FORMATS["fp32"])
    lt_s = key_a_s.as_unsigned() < key_b_s.as_unsigned()

    key_a_8, d_8 = _fp_to_key_for_fmt(a_8, FP_FORMATS["fp8_e4m3"])
    key_b_8, _   = _fp_to_key_for_fmt(b_8, FP_FORMATS["fp8_e4m3"])
    lt_8 = key_a_8.as_unsigned() < key_b_8.as_unsigned()

    key_a_4, d_4 = _fp_to_key_for_fmt(a_4, FP_FORMATS["fp4_e2m1"])
    key_b_4, _   = _fp_to_key_for_fmt(b_4, FP_FORMATS["fp4_e2m1"])
    lt_4 = key_a_4.as_unsigned() < key_b_4.as_unsigned()

    # 5:1 mux on fmt_sel (with 5..7 collapsed into lt_s).
    is_bf = fmt_sel == m.const(FMT_BF16,     width=FMT_SEL_W)
    is_h  = fmt_sel == m.const(FMT_FP16,     width=FMT_SEL_W)
    is_s  = fmt_sel == m.const(FMT_FP32,     width=FMT_SEL_W)
    is_8  = fmt_sel == m.const(FMT_FP8_E4M3, width=FMT_SEL_W)
    is_4  = fmt_sel == m.const(FMT_FP4_E2M1, width=FMT_SEL_W)

    # Build a balanced cascade: lt_bf when is_bf, else lt_h when is_h, else
    # lt_s when is_s, else lt_8 when is_8, else lt_4 (which also covers
    # reserved fmt_sel values 5..7 since none of the is_xxx flags fire).
    lt_4_or_default = is_4.select(lt_4, lt_s)
    lt_8_or_lt_4    = is_8.select(lt_8, lt_4_or_default)
    lt_s_or_lt_8    = is_s.select(lt_s, lt_8_or_lt_4)
    lt_h_or_lt_s    = is_h.select(lt_h, lt_s_or_lt_8)
    lt              = is_bf.select(lt_bf, lt_h_or_lt_s)

    cmp_depth = max(2, int(math.ceil(math.log2(VAL_W))))
    max_key_depth = max(d_bf, d_h, d_s, d_8, d_4)
    return lt, max_key_depth + cmp_depth + 5  # +5 ≈ five-level mux cascade
