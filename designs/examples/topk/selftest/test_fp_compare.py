"""Self-tests for ``fp_compare`` Python reference (``fp_lt_py``)."""
from __future__ import annotations

import random
import sys
from pathlib import Path

_TOPK = Path(__file__).resolve().parent.parent
if str(_TOPK) not in sys.path:
    sys.path.insert(0, str(_TOPK))

from tool import bits_to_float, float_to_bits, fp_lt_py, fp_to_unsigned_key
from topk_config import (
    FMT_BF16, FMT_FP16, FMT_FP32, FMT_FP8_E4M3, FMT_FP4_E2M1,
    fmt_of, fmt_of_sel,
)


def _random_range_for(fmt) -> float:
    if fmt.name == "fp32":
        return 1e3
    if fmt.name == "fp16":
        return 6e4
    if fmt.name == "bf16":
        return 1e3
    if fmt.name == "fp8_e4m3":
        return 256.0   # well below fp8_e4m3 max (≈448)
    return 4.0         # fp4_e2m1 max=6


def _big_for(fmt) -> tuple[float, float]:
    if fmt.name == "fp32":
        return 1e30, -1e30
    if fmt.name == "fp16":
        return 6e4, -6e4
    if fmt.name == "bf16":
        return 1e35, -1e35
    if fmt.name == "fp8_e4m3":
        return 192.0, -192.0
    return 4.0, -4.0


def run() -> None:
    rng = random.Random(0xCAFEBABE)

    # ── Random comparisons (5 fmts) ──
    for fmt_sel in [FMT_BF16, FMT_FP16, FMT_FP32, FMT_FP8_E4M3, FMT_FP4_E2M1]:
        fmt = fmt_of_sel(fmt_sel)
        rng_lim = _random_range_for(fmt)
        ok = 0
        n_trials = 2000 if fmt.name not in ("fp4_e2m1",) else 0
        for _ in range(n_trials):
            af = rng.uniform(-rng_lim, rng_lim)
            bf = rng.uniform(-rng_lim, rng_lim)
            ab = float_to_bits(af, fmt)
            bb = float_to_bits(bf, fmt)
            af_round = bits_to_float(ab, fmt)
            bf_round = bits_to_float(bb, fmt)
            if af_round == 0.0 and bf_round == 0.0:
                continue
            ref = 1 if af_round < bf_round else 0
            got = fp_lt_py(ab, bb, fmt_sel)
            assert got == ref, (
                f"fmt={fmt.name} a={af_round} b={bf_round} ref={ref} got={got}"
            )
            ok += 1

        # ── Special-case checks (only those that exist for the fmt) ──
        zero_p = float_to_bits(+0.0, fmt)
        zero_n = float_to_bits(-0.0, fmt)
        big_p_v, big_n_v = _big_for(fmt)
        big_p = float_to_bits(big_p_v, fmt)
        big_n = float_to_bits(big_n_v, fmt)
        n_special = 0
        # ±0 ordering: only meaningful if -0 is encodable distinctly. fp4_e2m1
        # has -0 (sign=1, exp=0, man=0).
        if zero_p != zero_n:
            assert fp_lt_py(zero_n, zero_p, fmt_sel) == 1
            assert fp_lt_py(zero_p, zero_n, fmt_sel) == 0
            n_special += 2

        if fmt.has_inf:
            inf_p = float_to_bits(float("inf"), fmt)
            inf_n = float_to_bits(float("-inf"), fmt)
            assert fp_lt_py(big_p, inf_p, fmt_sel) == 1
            assert fp_lt_py(inf_p, big_p, fmt_sel) == 0
            assert fp_lt_py(inf_n, big_n, fmt_sel) == 1
            assert fp_lt_py(big_n, inf_n, fmt_sel) == 0
            n_special += 4

        if fmt.has_nan and fmt.man_w >= 1:
            # NaN encoding depends on whether ±inf exists:
            #   IEEE-like (has_inf): exp=all-1, man != 0 → use mantissa = 1
            #   fp8_e4m3 (has_nan, no inf): lone NaN at S.1111.111 (full mantissa)
            if fmt.has_inf:
                nan_b = (fmt.exp_all_ones << fmt.man_w) | 1
            else:
                nan_b = (fmt.exp_all_ones << fmt.man_w) | fmt.man_all_ones
            assert fp_lt_py(nan_b, big_n, fmt_sel) == 1, "NaN should sort below any finite"
            assert fp_lt_py(big_n, nan_b, fmt_sel) == 0
            n_special += 2

        # neg_max_finite must beat (==loses to) every other finite value.
        nmf = fmt.neg_max_finite_bits
        max_pos = fmt.max_finite_pos_bits
        assert fp_lt_py(nmf, max_pos, fmt_sel) == 1
        assert fp_lt_py(max_pos, nmf, fmt_sel) == 0
        n_special += 2

        # subnormal smallest > +0 (only when format admits subnormals).
        if fmt.exp_w >= 2:
            subn = 1
            if subn != zero_p:
                assert fp_lt_py(zero_p, subn, fmt_sel) == 1
                assert fp_lt_py(subn, zero_p, fmt_sel) == 0
                n_special += 2

        print(f"  fp_lt {fmt.name:<10s}: {ok} random + {n_special} specials — OK")

    # ── fp4_e2m1: exhaustive 16x16 = 256 pairs ──
    fmt = fmt_of("fp4_e2m1")
    fmt_sel = FMT_FP4_E2M1
    fp4_total = 0
    for a in range(16):
        for b in range(16):
            ka = fp_to_unsigned_key(a, fmt)
            kb = fp_to_unsigned_key(b, fmt)
            ref = 1 if ka < kb else 0
            got = fp_lt_py(a, b, fmt_sel)
            assert got == ref, (
                f"fp4 exhaustive: a={a:#x} b={b:#x} ref={ref} got={got}"
            )
            fp4_total += 1
    print(f"  fp_lt fp4_e2m1   : {fp4_total} exhaustive pairs — OK")

    # ── fp8_e4m3: monotone-key sanity on 64 random + boundary patterns ──
    fmt = fmt_of("fp8_e4m3")
    fmt_sel = FMT_FP8_E4M3
    boundary_bits = [0x00, 0x80, 0x7F, 0xFF, 0x7E, 0xFE, 0x40, 0xC0]
    fp8_total = 0
    for a in boundary_bits:
        for b in boundary_bits:
            ka = fp_to_unsigned_key(a, fmt)
            kb = fp_to_unsigned_key(b, fmt)
            ref = 1 if ka < kb else 0
            got = fp_lt_py(a, b, fmt_sel)
            assert got == ref, f"fp8 boundary: a={a:#x} b={b:#x}"
            fp8_total += 1
    print(f"  fp_lt fp8_e4m3   : {fp8_total} boundary pairs — OK")


if __name__ == "__main__":
    print("Running fp_compare self-tests (Python reference)...")
    run()
    print("All fp_compare tests passed.")
