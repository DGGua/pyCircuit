"""Pure-Python helpers for the topk-histogram design.

Bit-level fp32 utilities, IEEE 754 corner-case enumerators, and random data
generators used by the software model and the testbenches.

All hardware-bit-exact software references live here (and in `fp_key.py`) so
the RTL output can be checked offline without numpy in the hot path.
"""
from __future__ import annotations

import math
import struct
from typing import Iterable, List


# ═════════════════════════════════════════════════════════════════
# fp32 ↔ bits
# ═════════════════════════════════════════════════════════════════

FP32_QNAN_CANONICAL = 0x7FC00000
FP32_POS_ZERO = 0x00000000
FP32_NEG_ZERO = 0x80000000
FP32_POS_INF = 0x7F800000
FP32_NEG_INF = 0xFF800000


def float_to_fp32_bits(f: float) -> int:
    """IEEE 754 single-precision encoding of ``f``."""
    return struct.unpack(">I", struct.pack(">f", f))[0]


def fp32_bits_to_float(bits: int) -> float:
    """Inverse of :func:`float_to_fp32_bits` for one 32-bit word."""
    return struct.unpack(">f", struct.pack(">I", bits & 0xFFFFFFFF))[0]


def is_fp32_nan_bits(bits: int) -> bool:
    """True iff the 32-bit pattern is a NaN encoding (exp=0xFF, mantissa!=0)."""
    bits &= 0xFFFFFFFF
    exp = (bits >> 23) & 0xFF
    man = bits & 0x7FFFFF
    return exp == 0xFF and man != 0


def fp32_special_values() -> List[int]:
    """Canonical fp32 corner bit patterns used by selftests."""
    return [
        FP32_POS_ZERO,
        FP32_NEG_ZERO,
        FP32_POS_INF,
        FP32_NEG_INF,
        FP32_QNAN_CANONICAL,
        0x7FC00001,                 # qNaN with extra payload
        0xFFC00001,                 # -qNaN
        0x7F800001,                 # sNaN
        0xFF800001,                 # -sNaN
        0x00000001,                 # smallest +subnormal
        0x80000001,                 # smallest -subnormal
        0x007FFFFF,                 # largest +subnormal
        0x807FFFFF,                 # largest -subnormal
        0x7F7FFFFF,                 # MAX +normal
        0xFF7FFFFF,                 # MAX -normal
        0x3F800000,                 # +1.0
        0xBF800000,                 # -1.0
    ]


# ═════════════════════════════════════════════════════════════════
# Bit packing helpers (lane bus <-> list of 32-bit words)
# ═════════════════════════════════════════════════════════════════

def pack_lanes(lanes: Iterable[int], lane_w: int) -> int:
    """Pack a sequence of lane values into one wide bus, lane 0 in the LSB.

    Matches the unpacking convention used by `_unpack_lanes` in the RTL
    helpers (`designs/examples/topk/topk.py:179-184`).
    """
    mask = (1 << lane_w) - 1
    bus = 0
    for i, v in enumerate(lanes):
        bus |= (int(v) & mask) << (i * lane_w)
    return bus


def unpack_lanes(bus: int, lane_w: int, n_lanes: int) -> List[int]:
    """Inverse of :func:`pack_lanes` — lane 0 is the LSB."""
    mask = (1 << lane_w) - 1
    return [(bus >> (i * lane_w)) & mask for i in range(n_lanes)]


# ═════════════════════════════════════════════════════════════════
# Stimulus generation
# ═════════════════════════════════════════════════════════════════

def gen_random_fp32(n: int, *, seed: int, include_specials: bool = True) -> List[int]:
    """Deterministic random fp32 bit pattern stream.

    Mixes a few canonical corner cases (NaN, ±0, ±Inf, subnormal) into the
    front when ``include_specials`` is True; the rest is uniformly random
    over the 32-bit space, filtered so we don't accidentally hit weird-but-
    rare patterns (sNaN payloads, etc.) more often than they would naturally.
    """
    import random

    rng = random.Random(seed)
    out: List[int] = []
    if include_specials:
        specials = fp32_special_values()
        out.extend(specials[: min(len(specials), n)])
    while len(out) < n:
        out.append(rng.getrandbits(32))
    return out[:n]


def split_into_beats(stream: Iterable[int], lanes_per_beat: int) -> List[List[int]]:
    """Chop a flat list of lane values into ``[beat][lane]`` rows.

    Pads the last beat with zeros if the total length is not a multiple of
    ``lanes_per_beat``.
    """
    flat = list(stream)
    n_beats = (len(flat) + lanes_per_beat - 1) // lanes_per_beat
    beats: List[List[int]] = []
    for b in range(n_beats):
        beat = flat[b * lanes_per_beat : (b + 1) * lanes_per_beat]
        if len(beat) < lanes_per_beat:
            beat = beat + [0] * (lanes_per_beat - len(beat))
        beats.append(beat)
    return beats


# ═════════════════════════════════════════════════════════════════
# Self-test
# ═════════════════════════════════════════════════════════════════

def _selftest() -> None:
    # Round-trip a few exactly-representable fp32 values + ±0 / ±Inf
    for f in (0.0, -0.0, 1.0, -1.0, 0.5, -0.5, 1024.0, -1024.0, float("inf"), -float("inf")):
        bits = float_to_fp32_bits(f)
        back = fp32_bits_to_float(bits)
        if math.isnan(f):
            assert math.isnan(back)
        else:
            assert back == f, f"fp32 round-trip failed for {f}: bits=0x{bits:08x} back={back}"
    # A double that does NOT exactly fit fp32 should produce a different float
    bits = float_to_fp32_bits(math.pi)
    back = fp32_bits_to_float(bits)
    assert abs(back - math.pi) < 1e-6 and back != math.pi
    assert bits == 0x40490FDB

    # NaN detection
    for nan_bits in (0x7FC00000, 0xFFC00000, 0x7F800001, 0x7FFFFFFF):
        assert is_fp32_nan_bits(nan_bits), f"missed NaN: 0x{nan_bits:08x}"
    for not_nan in (0x00000000, 0x80000000, 0x7F800000, 0xFF800000, 0x3F800000, 0x00000001):
        assert not is_fp32_nan_bits(not_nan), f"false NaN positive: 0x{not_nan:08x}"

    # Packing round-trip
    lanes = [i * 0x01010101 for i in range(8)]
    bus = pack_lanes(lanes, lane_w=32)
    assert unpack_lanes(bus, lane_w=32, n_lanes=8) == lanes

    # Random stream length
    s = gen_random_fp32(1024, seed=0)
    assert len(s) == 1024 and all(0 <= v < (1 << 32) for v in s)

    # Beat split (1024 lanes / 128 = 8 beats)
    beats = split_into_beats(s, lanes_per_beat=128)
    assert len(beats) == 8 and all(len(b) == 128 for b in beats)

    print("tool.py: selftest OK")


if __name__ == "__main__":
    _selftest()
