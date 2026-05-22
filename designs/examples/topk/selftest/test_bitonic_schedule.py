"""Self-tests for ``bitonic_schedule`` pure-Python schedule model."""
from __future__ import annotations

import math
import random
import sys
from pathlib import Path

_TOPK = Path(__file__).resolve().parent.parent
if str(_TOPK) not in sys.path:
    sys.path.insert(0, str(_TOPK))

from bitonic_schedule import (
    apply_half_merge_2p,
    apply_schedule,
    full_merge_2p_apply,
    gen_full_merge_2p_desc,
    gen_merge_half_schedule_2p,
    gen_sort_schedule_desc,
)


def test_known_n8_example() -> None:
    """Cross-check N=8 against a known random permutation."""
    sched = gen_sort_schedule_desc(8)
    arr = [3, 1, 4, 1, 5, 9, 2, 6]
    out = apply_schedule(arr, sched)
    assert out == sorted(arr, reverse=True), f"got {out}"
    print("  N=8 known example — OK")


def test_sort_random() -> None:
    rng = random.Random(0xC0FFEE)
    for N in [2, 4, 8, 16, 32, 64, 128, 256]:
        sched = gen_sort_schedule_desc(N)
        D = int(math.log2(N))
        expected_layers = D * (D + 1) // 2
        assert len(sched) == expected_layers, f"N={N}: layers={len(sched)}, expected {expected_layers}"
        for stride, pairs in sched:
            assert len(pairs) == N // 2
            seen = set()
            for lo, hi, _ in pairs:
                assert lo not in seen and hi not in seen
                seen.add(lo)
                seen.add(hi)
                assert hi - lo == stride
        for trial in range(20):
            arr = [rng.randint(-10000, 10000) for _ in range(N)]
            out = apply_schedule(arr, sched)
            assert out == sorted(arr, reverse=True), (
                f"N={N} trial={trial} failed:\n  in={arr}\n  out={out}\n  ref={sorted(arr, reverse=True)}"
            )
        total_pairs = sum(len(p) for _, p in sched)
        print(f"  sort N={N:4d}: layers={len(sched):3d}, pairs={total_pairs:5d} — OK")


def test_full_merge_2p_random() -> None:
    rng = random.Random(0xBADF00D)
    for P in [1, 2, 4, 8, 16, 32, 64, 128, 256]:
        sched = gen_full_merge_2p_desc(P)
        assert len(sched) == int(math.log2(P)) + 1
        for trial in range(10):
            A = sorted([rng.randint(-1000, 1000) for _ in range(P)], reverse=True)
            B = sorted([rng.randint(-1000, 1000) for _ in range(P)], reverse=True)
            out = full_merge_2p_apply(A, B)
            ref = sorted(A + B, reverse=True)
            assert out == ref, f"P={P} trial={trial} failed"
        print(f"  full_merge_2p P={P:3d}: layers={len(sched)} — OK")


def test_merge_half_schedule_2p_random() -> None:
    """Verify ``gen_merge_half_schedule_2p`` produces an equal-pairs split.

    Each full merge layer (P pairs) must be split into two half-layers of
    exactly P/2 pairs each. The combined sequence (both halves applied in
    order) must produce the same result as the full schedule.
    """
    rng = random.Random(0xBEEFCAFE)
    for P in [2, 4, 8, 16, 32, 64, 128, 256]:
        full_sched = gen_full_merge_2p_desc(P)
        half_sched = gen_merge_half_schedule_2p(P)
        assert len(half_sched) == 2 * len(full_sched), (
            f"P={P}: half-layers={len(half_sched)}, expected {2*len(full_sched)}"
        )
        # Each half-layer has exactly P/2 pairs.
        expected_pairs = max(1, P // 2)
        for hi, (_stride, pairs) in enumerate(half_sched):
            assert len(pairs) == expected_pairs, (
                f"P={P} half[{hi}] has {len(pairs)} pairs, expected {expected_pairs}"
            )
            # All lane indices within the half are disjoint (no double-touch).
            seen: set[int] = set()
            for lo, hi_idx, _d in pairs:
                assert lo not in seen and hi_idx not in seen, (
                    f"P={P} half[{hi}] reuses lane index"
                )
                seen.add(lo)
                seen.add(hi_idx)

        # Apply the half-schedule — it must sort A++reversed(B) into descending.
        for trial in range(10):
            A = sorted([rng.randint(-1000, 1000) for _ in range(P)], reverse=True)
            B = sorted([rng.randint(-1000, 1000) for _ in range(P)], reverse=True)
            out = apply_half_merge_2p(A, B)
            ref = sorted(A + B, reverse=True)
            assert out == ref, f"P={P} trial={trial} half-merge failed"
        total_pairs = sum(len(p) for _, p in half_sched)
        print(f"  half_merge P={P:3d}: half-layers={len(half_sched):2d}, pairs={total_pairs:5d} — OK")


def test_engine_software_model() -> None:
    """Cross-check ``simulate_engine_python`` mirrors a python golden across fmts."""
    import sys
    from pathlib import Path
    _TOPK = Path(__file__).resolve().parent.parent
    if str(_TOPK) not in sys.path:
        sys.path.insert(0, str(_TOPK))

    from topk import simulate_engine_topk_keys
    from topk_config import fmt_of
    from tool import float_to_bits, fp_to_unsigned_key

    rng = random.Random(0xDEADBEEF)
    for fmt_name in ["bf16", "fp16", "fp32", "fp8_e4m3", "fp4_e2m1"]:
        fmt = fmt_of(fmt_name)
        for P, K_MAX, K, n_chunks in [(4, 16, 4, 4), (4, 16, 7, 8), (8, 32, 16, 6)]:
            if fmt_name == "fp4_e2m1":
                pool = [-6, -4, -3, -2, -1.5, -1, -0.5, 0,
                         0.5, 1, 1.5, 2, 3, 4, 6]
                gen = lambda: rng.choice(pool)
            elif fmt_name == "fp8_e4m3":
                gen = lambda: rng.uniform(-128, 128)
            else:
                gen = lambda: rng.uniform(-100, 100)
            chunks = []
            all_pairs = []
            for c in range(n_chunks):
                vals = [gen() for _ in range(P)]
                bits = [float_to_bits(v, fmt) for v in vals]
                idxs = list(range(c * P, (c + 1) * P))
                chunks.append(list(zip(bits, idxs)))
                all_pairs.extend(zip(bits, idxs))
            result = simulate_engine_topk_keys(chunks, K=K, P=P, K_MAX=K_MAX, fmt=fmt)
            golden = sorted(all_pairs,
                            key=lambda p: -fp_to_unsigned_key(p[0], fmt))[:K]
            rk = [fp_to_unsigned_key(r[0], fmt) for r in result]
            gk = [fp_to_unsigned_key(g[0], fmt) for g in golden]
            assert rk == gk, (
                f"engine sw model {fmt_name} P={P} K={K} K_MAX={K_MAX} N={n_chunks*P} mismatch"
            )
        print(f"  engine sw {fmt_name:<10s}: 3 cases — OK")


def run() -> None:
    test_known_n8_example()
    test_sort_random()
    test_full_merge_2p_random()
    test_merge_half_schedule_2p_random()
    test_engine_software_model()


if __name__ == "__main__":
    print("Running bitonic_schedule self-tests...")
    run()
    print("All bitonic_schedule tests passed.")
