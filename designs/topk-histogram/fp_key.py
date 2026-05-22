"""fp32 ↔ sortable key transform — software model and RTL helpers.

Implements the contract from
[designs/topk-histogram/arch.md](designs/topk-histogram/arch.md) §4.2.3 / §4.2.4:

    fp_to_key:
        if (x == 0x7FC00000)  return 0xFFFFFFFF;          // canonical qNaN → 顶
        if (x[31] == 0)       return x ^ 0x80000000;      // 正数 / +0 / +Inf / +subnormal
        else                  return ~x;                  // 负数 / -0 / -Inf / -subnormal / -NaN

    key_to_fp:
        if (k[31] == 1)       return k ^ 0x80000000;      // 原本是正数 / +0 / +Inf / NaN
        else                  return ~k;                  // 原本是负数 / -0 / -Inf

Properties:
- Finite values round-trip bit-exact.
- ``a < b`` (in arch §4.2.4 sortable-key ordering) iff
  ``fp_to_key_py(a_bits) < fp_to_key_py(b_bits)`` as unsigned 32-bit.
- NaN bit-pattern is NOT preserved end-to-end (canonical qNaN is folded to
  the top of the key space and back-converts to ``0x7FFFFFFF``), but the
  "is NaN" property is preserved.

The Python functions here back the software model (Layer A); the
``fp32_to_sortable_key_hw`` / ``sortable_key_to_fp32_hw`` builders below are
the pycircuit-side Wire builders that produce the equivalent 128-lane comb
logic for the RTL path.
"""
from __future__ import annotations

from typing import TYPE_CHECKING

from tool import (
    FP32_QNAN_CANONICAL,
    fp32_bits_to_float,
    fp32_special_values,
    is_fp32_nan_bits,
)

if TYPE_CHECKING:  # avoid hard dependency at Layer A self-test time
    from pycircuit import CycleAwareCircuit
    from pycircuit.hw import Wire


# ═════════════════════════════════════════════════════════════════
# Python model
# ═════════════════════════════════════════════════════════════════

_W = 32
_MASK = (1 << _W) - 1
_SIGN_BIT_MASK = 0x80000000
_TOP_KEY = 0xFFFFFFFF


def fp_to_key_py(bits: int) -> int:
    """Python reference for `fp_to_key` (arch §4.2.4)."""
    bits &= _MASK
    if bits == FP32_QNAN_CANONICAL:
        return _TOP_KEY
    if (bits >> 31) == 0:
        return bits ^ _SIGN_BIT_MASK
    return (~bits) & _MASK


def key_to_fp_py(key: int) -> int:
    """Python reference for `key_to_fp` (arch §4.2.3)."""
    key &= _MASK
    if (key >> 31) == 1:
        return key ^ _SIGN_BIT_MASK
    return (~key) & _MASK


def fp_to_key_lanes_py(lane_bits: list[int]) -> list[int]:
    """Apply :func:`fp_to_key_py` to each lane."""
    return [fp_to_key_py(b) for b in lane_bits]


def key_to_fp_lanes_py(lane_keys: list[int]) -> list[int]:
    """Apply :func:`key_to_fp_py` to each lane."""
    return [key_to_fp_py(k) for k in lane_keys]


# ═════════════════════════════════════════════════════════════════
# pycircuit / RTL builders
# ═════════════════════════════════════════════════════════════════

def fp32_to_sortable_key_hw(m: "CycleAwareCircuit", x: "Wire") -> "Wire":
    """Combinational helper: fp32 → sortable key (one lane).

    Matches :func:`fp_to_key_py`. Resource: ~10 LUT/lane (with the explicit
    canonical-qNaN branch). Drop the first ``==`` if upstream guarantees NaN
    cannot appear (saves ~0.5K LUT across 128 lanes).
    """
    assert x.width == _W, f"fp32_to_sortable_key_hw expects 32-bit lane (got {x.width})"
    qnan = m.const(FP32_QNAN_CANONICAL, width=_W)
    sign_mask = m.const(_SIGN_BIT_MASK, width=_W)
    top = m.const(_TOP_KEY, width=_W)
    is_qnan = x == qnan
    sign = x.slice(lsb=31, width=1)
    pos_key = x ^ sign_mask
    neg_key = ~x
    keyed = sign.select(neg_key, pos_key)
    return is_qnan.select(top, keyed)


def sortable_key_to_fp32_hw(m: "CycleAwareCircuit", k: "Wire") -> "Wire":
    """Combinational helper: sortable key → fp32 (one lane).

    Matches :func:`key_to_fp_py`. Resource: ~5 LUT/lane.
    """
    assert k.width == _W, f"sortable_key_to_fp32_hw expects 32-bit lane (got {k.width})"
    sign_mask = m.const(_SIGN_BIT_MASK, width=_W)
    sign = k.slice(lsb=31, width=1)
    return sign.select(k ^ sign_mask, ~k)


# ═════════════════════════════════════════════════════════════════
# Self-test
# ═════════════════════════════════════════════════════════════════

def _selftest() -> None:
    # 1) Finite round-trip is bit-exact
    finite_specials = [
        0x00000000, 0x80000000, 0x7F800000, 0xFF800000,  # ±0, ±Inf
        0x00000001, 0x80000001, 0x007FFFFF, 0x807FFFFF,  # ±subnormal
        0x7F7FFFFF, 0xFF7FFFFF,                          # ±MAX_FINITE
        0x3F800000, 0xBF800000,                          # ±1.0
    ]
    for bits in finite_specials:
        k = fp_to_key_py(bits)
        back = key_to_fp_py(k)
        assert back == bits, (
            f"finite round-trip broken for 0x{bits:08x}: "
            f"key=0x{k:08x} back=0x{back:08x}"
        )

    # 2) NaN "is NaN" property is preserved; bit pattern may change
    for nan_bits in (0x7FC00000, 0xFFC00000, 0x7F800001, 0xFF800001, 0x7FFFFFFF):
        k = fp_to_key_py(nan_bits)
        back = key_to_fp_py(k)
        assert is_fp32_nan_bits(back), (
            f"NaN-ness lost on round-trip: 0x{nan_bits:08x} → key=0x{k:08x} back=0x{back:08x}"
        )

    # 3) Canonical qNaN is at the top of the key space
    assert fp_to_key_py(FP32_QNAN_CANONICAL) == 0xFFFFFFFF

    # 4) Monotone ordering check (unsigned key < ↔ fp value <, modulo NaN folding)
    import math
    pairs = [
        (-1e30, 1e30),
        (-1.0, 1.0),
        (-0.0, 1e-30),         # -0 < +tiny
        (1.0, 2.0),
        (-2.0, -1.0),
        (-float("inf"), -1e30),
        (1e30, float("inf")),
    ]
    for a, b in pairs:
        a_bits = _fp32_bits_of(a)
        b_bits = _fp32_bits_of(b)
        ka = fp_to_key_py(a_bits)
        kb = fp_to_key_py(b_bits)
        assert ka < kb, (
            f"monotone broken: {a} ({fp32_bits_to_float(a_bits)}) → 0x{ka:08x}, "
            f"{b} ({fp32_bits_to_float(b_bits)}) → 0x{kb:08x}"
        )

    # 5) Every fp32 special maps to *some* key in [0, 2^32)
    for bits in fp32_special_values():
        k = fp_to_key_py(bits)
        assert 0 <= k <= _MASK

    # 6) +Inf < canonical qNaN < (nothing above qNaN)
    assert fp_to_key_py(0x7F800000) < fp_to_key_py(FP32_QNAN_CANONICAL)

    # 7) -Inf < -MAX_FINITE in key space
    assert fp_to_key_py(0xFF800000) < fp_to_key_py(0xFF7FFFFF)

    # 8) Lanes wrapper
    keys = fp_to_key_lanes_py(finite_specials)
    backs = key_to_fp_lanes_py(keys)
    assert backs == finite_specials

    print("fp_key.py: selftest OK")


def _fp32_bits_of(f: float) -> int:
    """Local helper to avoid pulling in numpy."""
    import struct
    return struct.unpack(">I", struct.pack(">f", f))[0]


if __name__ == "__main__":
    _selftest()
