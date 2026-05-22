"""Pure-Python helpers for topk (reference compare, float ↔ bits, special values).

Used by testbenches, ``selftest/``, and the software golden model — not by
hardware paths (those live in ``fp_compare.fp_lt``). All hardware-bit-exact
software references live here so the RTL output can be checked offline.

Format coverage:

    bf16, fp16, fp32, fp8_e4m3, fp4_e2m1

For fp8_e4m3 and fp4_e2m1 we hand-roll ``float ↔ bits`` since numpy has no
native dtypes for them.
"""
from __future__ import annotations

import math
import struct

from topk_config import FpFormat, fmt_of, fmt_of_sel


# ═════════════════════════════════════════════════════════════════
# NaN detection (per fmt)
# ═════════════════════════════════════════════════════════════════

def _is_nan_bits(bits: int, fmt: FpFormat) -> bool:
    """True iff ``bits`` is a NaN encoding for the given format.

    Per-format rules:
      - IEEE-like (bf16/fp16/fp32): exp_all_ones AND man != 0.
      - fp8_e4m3 (no ±inf, single NaN):  S.1111.111 — exp_all_ones AND man=man_all_ones.
      - fp4_e2m1 (no NaN at all):  always False.
    """
    if not fmt.has_nan:
        return False
    bits &= (1 << fmt.width) - 1
    exp = (bits >> fmt.man_w) & ((1 << fmt.exp_w) - 1)
    man = bits & ((1 << fmt.man_w) - 1)
    if fmt.has_inf:
        return exp == fmt.exp_all_ones and man != 0
    return exp == fmt.exp_all_ones and man == fmt.man_all_ones


# ═════════════════════════════════════════════════════════════════
# Monotone-key transform (NaN-fold + sign-magnitude → unsigned)
# ═════════════════════════════════════════════════════════════════

def fp_to_unsigned_key(bits: int, fmt: FpFormat) -> int:
    """Map an IEEE-754-style bit pattern to a monotone unsigned key.

    Property:
        ``fp_to_unsigned_key(a, fmt) < fp_to_unsigned_key(b, fmt)``
            ↔ ``float_of(a, fmt) < float_of(b, fmt)``
    in the NaN-folded ordering (NaN < every finite, even -inf if the format
    has it; otherwise NaN < every finite via NEG_MAX_FINITE substitution).
    Tie-break on equal floats matches RTL's monotone-key compare.
    """
    mask = (1 << fmt.width) - 1
    bits &= mask
    if _is_nan_bits(bits, fmt):
        bits = fmt.neg_inf_bits & mask
    sign = (bits >> fmt.sign_bit) & 1
    if sign == 0:
        return bits ^ (1 << fmt.sign_bit)
    return (~bits) & mask


def fp_lt_py(a_bits: int, b_bits: int, fmt_sel: int) -> int:
    """Reference: 1 if a < b under runtime fmt_sel (NaN-folded ordering)."""
    fmt = fmt_of_sel(fmt_sel)
    mask = (1 << fmt.width) - 1
    return (
        1 if fp_to_unsigned_key(a_bits & mask, fmt)
              < fp_to_unsigned_key(b_bits & mask, fmt) else 0
    )


# ═════════════════════════════════════════════════════════════════
# IEEE FP32 reference (used as the reference for hand-rolled fmts)
# ═════════════════════════════════════════════════════════════════

def _float_to_fp32_bits(f: float) -> int:
    return struct.unpack(">I", struct.pack(">f", f))[0]


def _fp32_bits_to_float(bits: int) -> float:
    return struct.unpack(">f", struct.pack(">I", bits & 0xFFFFFFFF))[0]


# ═════════════════════════════════════════════════════════════════
# Hand-rolled fp8_e4m3 (OCP 8-bit FP)
# ═════════════════════════════════════════════════════════════════
# Format: 1 sign + 4 exp + 3 mantissa, bias=7. No ±inf. Exactly one NaN
# encoding: S.1111.111 (positive and negative NaN both round-trip to the
# same canonical pattern 0x7F).

_FP8_E4M3_MAX = 448.0     # = 1.75 * 2^8 = 448; encoding 0b0_1111_110 = 0x7E


def _float_to_fp8_e4m3_bits(f: float) -> int:
    if math.isnan(f):
        return 0x7F  # canonical NaN encoding (positive)
    if math.isinf(f):
        # No infinity in E4M3 — saturate to ±MAX_FINITE.
        return 0x7E if f > 0 else 0xFE
    sign = 1 if math.copysign(1.0, f) < 0 else 0
    af = abs(f)
    if af == 0.0:
        return sign << 7
    # Saturate
    if af > _FP8_E4M3_MAX:
        return (sign << 7) | 0x7E
    # Find unbiased exponent + mantissa
    exp_unb = math.floor(math.log2(af))
    mant_f = af / (2.0 ** exp_unb)
    e = exp_unb + 7  # bias=7
    if e <= 0:
        # Subnormal
        # value = mant * 2^(1 - bias) where mant is M / 2^M_W
        mant_int = int(round(af / (2.0 ** (1 - 7 - 3))))  # = af / 2^(-9)
        if mant_int >= (1 << 3):
            # rounded up into normal range
            return (sign << 7) | (1 << 3)
        return (sign << 7) | mant_int
    if e >= 16:
        # Overflow saturate
        return (sign << 7) | 0x7E
    # Normal
    mant_int = int(round((mant_f - 1.0) * (1 << 3)))
    if mant_int >= (1 << 3):
        e += 1
        mant_int = 0
        if e >= 16:
            return (sign << 7) | 0x7E
    bits = (sign << 7) | (e << 3) | (mant_int & 0x7)
    # Block the lone NaN encoding: round it to the nearest non-NaN
    if bits == 0x7F:
        bits = 0x7E
    if bits == 0xFF:
        bits = 0xFE
    return bits


def _fp8_e4m3_bits_to_float(bits: int) -> float:
    bits &= 0xFF
    sign = (bits >> 7) & 1
    exp = (bits >> 3) & 0xF
    man = bits & 0x7
    sig = -1.0 if sign else 1.0
    # NaN encoding
    if exp == 0xF and man == 0x7:
        return float("nan")
    if exp == 0:
        # Subnormal: value = sig * man / 2^3 * 2^(1-bias) = sig * man * 2^(-9)
        return sig * man * (2.0 ** -9)
    # Normal: value = sig * (1 + man/8) * 2^(exp-7)
    return sig * (1.0 + man / 8.0) * (2.0 ** (exp - 7))


# ═════════════════════════════════════════════════════════════════
# Hand-rolled fp4_e2m1 (MXFP4)
# ═════════════════════════════════════════════════════════════════
# Format: 1 sign + 2 exp + 1 mantissa, bias=1. No ±inf, no NaN.
# All 16 bit patterns are valid finite values.
#
#   bits          binary       value
#   0  / 8       0_00_0 / 1_00_0   ±0
#   1  / 9       0_00_1 / 1_00_1   ±0.5  (subnormal: 1 * 2^(1-1-1) = 0.5)
#   2  / A       0_01_0 / 1_01_0   ±1.0
#   3  / B       0_01_1 / 1_01_1   ±1.5
#   4  / C       0_10_0 / 1_10_0   ±2.0
#   5  / D       0_10_1 / 1_10_1   ±3.0
#   6  / E       0_11_0 / 1_11_0   ±4.0
#   7  / F       0_11_1 / 1_11_1   ±6.0

_FP4_E2M1_TABLE = [
    0.0, 0.5, 1.0, 1.5, 2.0, 3.0, 4.0, 6.0,
    -0.0, -0.5, -1.0, -1.5, -2.0, -3.0, -4.0, -6.0,
]
_FP4_E2M1_MAX = 6.0


def _float_to_fp4_e2m1_bits(f: float) -> int:
    if math.isnan(f):
        # No NaN encoding; spec says NaN handling is implementation-defined.
        # We saturate to NEG_MAX_FINITE (= -6.0 = bits 0xF) so it always loses.
        return 0xF
    sign = 1 if math.copysign(1.0, f) < 0 else 0
    af = abs(f)
    if af == 0.0:
        return sign << 3
    if math.isinf(f) or af > _FP4_E2M1_MAX:
        return (sign << 3) | 0x7   # ±6.0
    # Search the positive table for the closest value (round-to-nearest-even
    # against this small table).
    best_idx = 0
    best_dist = float("inf")
    for i in range(8):
        d = abs(af - _FP4_E2M1_TABLE[i])
        if d < best_dist or (d == best_dist and (i & 1) == 0):
            best_dist = d
            best_idx = i
    return (sign << 3) | best_idx


def _fp4_e2m1_bits_to_float(bits: int) -> float:
    return _FP4_E2M1_TABLE[bits & 0xF]


# ═════════════════════════════════════════════════════════════════
# Public dispatch: float ↔ bits / specials per fmt
# ═════════════════════════════════════════════════════════════════

def float_to_bits(f: float, fmt: FpFormat) -> int:
    """Convert Python float to the requested fp format bit pattern."""
    if fmt.name == "fp32":
        return _float_to_fp32_bits(f)
    if fmt.name == "bf16":
        return _float_to_fp32_bits(f) >> 16
    if fmt.name == "fp16":
        import numpy as np
        return int(np.frombuffer(np.float16(f).tobytes(), dtype=np.uint16)[0])
    if fmt.name == "fp8_e4m3":
        return _float_to_fp8_e4m3_bits(f)
    if fmt.name == "fp4_e2m1":
        return _float_to_fp4_e2m1_bits(f)
    raise ValueError(f"unknown fmt {fmt.name}")


def bits_to_float(bits: int, fmt: FpFormat) -> float:
    if fmt.name == "fp32":
        return _fp32_bits_to_float(bits)
    if fmt.name == "bf16":
        return _fp32_bits_to_float((bits & 0xFFFF) << 16)
    if fmt.name == "fp16":
        import numpy as np
        return float(np.frombuffer(int(bits).to_bytes(2, "little"), dtype=np.float16)[0])
    if fmt.name == "fp8_e4m3":
        return _fp8_e4m3_bits_to_float(bits)
    if fmt.name == "fp4_e2m1":
        return _fp4_e2m1_bits_to_float(bits)
    raise ValueError(f"unknown fmt {fmt.name}")


def neg_max_finite_bits(fmt: FpFormat) -> int:
    """Most-negative finite bit pattern for ``fmt``."""
    return fmt.neg_max_finite_bits


def neg_inf_or_neg_max_finite_bits(fmt: FpFormat) -> int:
    """Lose-anything sentinel: ±inf when the fmt has it, else NEG_MAX_FINITE."""
    return fmt.neg_inf_bits


def gen_special_values(fmt: FpFormat) -> list[int]:
    """A short list of canonical special bit patterns to feed into selftests.

    Always includes: +0, -0, NEG_MAX_FINITE, MAX_FINITE_POS, smallest +subnorm.
    Adds NaN/±inf encodings only when the format supports them.
    """
    specials = [
        0,                                           # +0
        1 << fmt.sign_bit,                           # -0
        fmt.max_finite_pos_bits,                     # MAX_FINITE_POS
        fmt.neg_max_finite_bits,                     # NEG_MAX_FINITE
        1,                                           # smallest +subnormal
        (1 << fmt.sign_bit) | 1,                     # smallest -subnormal (== -smallest +sub)
    ]
    if fmt.has_inf:
        specials.append(fmt.exp_all_ones << fmt.man_w)             # +inf
        specials.append((1 << fmt.sign_bit) | (fmt.exp_all_ones << fmt.man_w))  # -inf
    if fmt.has_nan:
        if fmt.has_inf:
            # IEEE-like: pick a nonzero-mantissa NaN encoding.
            specials.append((fmt.exp_all_ones << fmt.man_w) | 1)               # +NaN (qNaN-like)
            specials.append((1 << fmt.sign_bit) | (fmt.exp_all_ones << fmt.man_w) | 1)  # -NaN
        else:
            # fp8_e4m3: lone NaN encoding S.1111.111
            specials.append((fmt.exp_all_ones << fmt.man_w) | fmt.man_all_ones)
            specials.append((1 << fmt.sign_bit) | (fmt.exp_all_ones << fmt.man_w) | fmt.man_all_ones)
    return specials
