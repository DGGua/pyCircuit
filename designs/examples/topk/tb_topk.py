"""Top-K cycle-accurate testbench (drives the unified engine RTL).

Mirrors the structure of ``designs/examples/counter/tb_counter.py``: builds
the ``topk.build`` module, drives stimuli into the DUT through ``CycleAwareTb``,
and ``expect``s the DUT outputs cycle-by-cycle against a Python golden.

DUT footprint (from ``topk_config.DEFAULT_PARAMS``):

    P = 4, K_MAX = 16, idx_w = 4
    L_S = 3 sort layers, L_M = 6 merge half-layers
    cycles per chunk (rows_used=1) = SORT(L_S=3) + PRE(1) + HP(L_M=6) + POST(1) = 11
    fire-to-IDLE = 12 cycles (state observed one cycle after each transition)

Test scenario
-------------
Two chunks of P=4 elements (fp32, K_MAX=16, K=4 → ``rows_used=1``)::

    chunk 0 vals : [3.0, 1.0, 4.0, 7.0]   idxs : [0, 1, 2, 3]
    chunk 1 vals : [5.0, 9.0, 2.0, 6.0]   idxs : [4, 5, 6, 7]

Top-4 (descending)::

    vals : [9.0, 7.0, 6.0, 5.0]           idxs : [5, 3, 7, 4]

Cycle plan (unified engine; ready_out=1 only when in IDLE)::

    cyc  0 : drive chunk 0, valid_in=1   (ready_out=1 → fire)
    cyc 1..11 : engine busy on chunk 0 (SORT → PRE → HP → POST → IDLE @ cy 12)
    cyc 12 : drive chunk 1, valid_in=1   (ready_out=1 again → fire)
    cyc 13..23 : engine busy on chunk 1
    cyc 24 : IDLE (ready_out=1, mem[0] holds final top-K)
    cyc 26+ : sram_rdata reflects mem[drain_addr=0] (raddr→raddr_d delays 1 cy
              past IDLE entry); we sample at cyc 30 for safety.

The Python software model from this file (``sw_topk_unified``) is also kept
and exercised by ``run_matrix`` / ``run_edge_cases``; invoke with
``python tb_topk.py --algo`` to run those checks against numpy.
"""
from __future__ import annotations

import math
import random
import sys
from pathlib import Path

from pycircuit import CycleAwareTb, Tb, compile_cycle_aware, testbench

_THIS_DIR = Path(__file__).resolve().parent
if str(_THIS_DIR) not in sys.path:
    sys.path.insert(0, str(_THIS_DIR))

from topk import build  # noqa: E402
from topk_config import (  # noqa: E402
    DEFAULT_PARAMS,
    TB_PRESETS,
    FMT_BF16, FMT_FP16, FMT_FP32,
    fmt_of,
)
from bitonic_schedule import (  # noqa: E402
    apply_schedule,
    full_merge_2p_apply,
    gen_full_merge_2p_desc,
    gen_sort_schedule_desc,
)
from tool import (  # noqa: E402
    bits_to_float,
    float_to_bits,
    fp_to_unsigned_key,
)


# ═════════════════════════════════════════════════════════════════
# Software model: chunk-by-chunk top-K mirroring the unified RTL
# (kept for golden computation in the RTL tb + ``--algo`` matrix run)
# ═════════════════════════════════════════════════════════════════

def sw_stage_a_full_p_sort(chunk_keys: list[int], P: int) -> list[int]:
    """Stage A: full P-element descending bitonic sort."""
    assert len(chunk_keys) == P
    return apply_schedule(chunk_keys, gen_sort_schedule_desc(P))


def sw_stage_b_streaming(
    running_rows: list[list[int]],
    init_done: list[bool],
    chunk_sorted_p: list[int],
    rows_used: int,
    P: int,
    neg_inf_key: int,
) -> tuple[list[list[int]], list[bool]]:
    """Streaming Stage B step with init_done bit-vector (matches RTL)."""
    n_rows_max = len(running_rows)
    assert len(chunk_sorted_p) == P
    assert 1 <= rows_used <= n_rows_max
    new_rows = list(running_rows)
    new_init = list(init_done)
    carry = list(chunk_sorted_p)
    for r in range(rows_used):
        if new_init[r]:
            row = new_rows[r]
        else:
            row = [neg_inf_key] * P
        merged = full_merge_2p_apply(row, carry)
        new_rows[r] = merged[:P]
        carry = merged[P:]
        new_init[r] = True
    return new_rows, new_init


def sw_topk_unified(
    chunks_keys: list[list[int]],
    K: int,
    P: int,
    K_MAX: int,
    neg_inf_key: int,
) -> list[int]:
    """End-to-end unified Top-K software model (returns top-K keys desc)."""
    n_rows_max = K_MAX // P
    rows_used = (K + P - 1) // P
    assert 1 <= rows_used <= n_rows_max
    running = [[0] * P for _ in range(n_rows_max)]
    init_done = [False] * n_rows_max
    for ch in chunks_keys:
        top_p = sw_stage_a_full_p_sort(ch, P)
        running, init_done = sw_stage_b_streaming(
            running, init_done, top_p, rows_used, P, neg_inf_key,
        )
    flat = [v for row in running[:rows_used] for v in row]
    return flat[:K]


def _apply_layer_pairs_hw(
    arr: list[tuple[int, int]],
    layer,
    fmt,
) -> list[tuple[int, int]]:
    """Apply one bitonic layer to a list of (val_bits, idx) pairs,
    matching the RTL CMP_SWAP tie-break exactly.

    HW: ``swap = ~(dir ^ lt)`` where ``lt = key_a < key_b`` in monotone
    unsigned space. On ties (lt=0) DESC keeps lane A at lo; ASC swaps.
    Python's ``sorted`` would compare tuples lexicographically (key, idx)
    on ties, which doesn't match — hence this explicit walk.
    """
    out = list(arr)
    _, layer_pairs = layer
    for lo, hi, direction in layer_pairs:
        a = out[lo]
        b = out[hi]
        ka = fp_to_unsigned_key(a[0], fmt)
        kb = fp_to_unsigned_key(b[0], fmt)
        lt = ka < kb
        swap = lt if direction == 1 else not lt
        if swap:
            out[lo], out[hi] = b, a
        else:
            out[lo], out[hi] = a, b
    return out


def _sort_desc_pairs_hw(
    seq: list[tuple[int, int]], P: int, fmt,
) -> list[tuple[int, int]]:
    """Stage A bit-exact: full P-element bitonic sort, descending."""
    sched = gen_sort_schedule_desc(P)
    out = list(seq)
    for layer in sched:
        out = _apply_layer_pairs_hw(out, layer, fmt)
    return out


def _full_merge_2p_pairs_hw(
    A: list[tuple[int, int]],
    B: list[tuple[int, int]],
    fmt,
) -> list[tuple[int, int]]:
    """Stage B bit-exact: full 2P → 2P bitonic merge, descending.

    Wiring convention matches ``bitonic_merge_2p_full``: the laned input
    is ``A ++ reversed(B)`` so the concat is valley bitonic.
    """
    P = len(A)
    laned = list(A) + list(reversed(B))
    sched = gen_full_merge_2p_desc(P)
    out = list(laned)
    for layer in sched:
        out = _apply_layer_pairs_hw(out, layer, fmt)
    return out


def sw_topk_unified_pairs(
    chunks_pairs: list[list[tuple[int, int]]],
    K: int,
    P: int,
    K_MAX: int,
    fmt,
) -> list[tuple[int, int]]:
    """Like sw_topk_unified but tracks (val_bits, idx) pairs.

    Operates exactly the same as the RTL: comparators live in monotone-key
    space, but the data carried through the pipe is the original bit
    pattern + index. Tie-breaks follow the bitonic network's CMP_SWAP
    semantics (so bf16/fp16 chunks with duplicate values match the RTL
    bit-for-bit on indices, not just on values).
    """
    n_rows_max = K_MAX // P
    rows_used = (K + P - 1) // P
    neg_inf_pair: tuple[int, int] = (fmt.neg_inf_bits, 0)

    running: list[list[tuple[int, int]]] = [[neg_inf_pair] * P for _ in range(n_rows_max)]
    init_done = [False] * n_rows_max

    neg_inf_row: list[tuple[int, int]] = [neg_inf_pair] * P
    for ch in chunks_pairs:
        sorted_ch = _sort_desc_pairs_hw(list(ch), P, fmt)
        carry: list[tuple[int, int]] = sorted_ch
        for r in range(rows_used):
            row: list[tuple[int, int]] = running[r] if init_done[r] else list(neg_inf_row)
            merged = _full_merge_2p_pairs_hw(row, carry, fmt)
            running[r] = merged[:P]
            carry = merged[P:]
            init_done[r] = True

    flat = [pair for row in running[:rows_used] for pair in row]
    return flat[:K]


# ═════════════════════════════════════════════════════════════════
# Bus packing helpers (lane 0 in LSB; matches topk._pack_lanes)
# ═════════════════════════════════════════════════════════════════

def pack_lanes(lanes: list[int], lane_w: int) -> int:
    bus = 0
    for i, v in enumerate(lanes):
        mask = (1 << lane_w) - 1
        bus |= (int(v) & mask) << (i * lane_w)
    return bus


# ═════════════════════════════════════════════════════════════════
# RTL testbench (counter-style)
# ═════════════════════════════════════════════════════════════════

@testbench
def tb(t: Tb) -> None:
    tb = CycleAwareTb(t)
    p = TB_PRESETS["smoke"]
    P     = int(DEFAULT_PARAMS["P"])
    K_MAX = int(DEFAULT_PARAMS["K_MAX"])
    idx_w = int(DEFAULT_PARAMS["idx_w"])
    val_w = 32
    K     = 4                                # runtime K (rows_used = ceil(K/P) = 1)
    fmt   = fmt_of("fp32")
    fmt_sel_value = FMT_FP32

    # ── Test vectors ──
    chunk0_vals = [3.0, 1.0, 4.0, 7.0]
    chunk0_idxs = [0, 1, 2, 3]
    chunk1_vals = [5.0, 9.0, 2.0, 6.0]
    chunk1_idxs = [4, 5, 6, 7]

    chunk0_bits = [float_to_bits(v, fmt) for v in chunk0_vals]
    chunk1_bits = [float_to_bits(v, fmt) for v in chunk1_vals]

    # ── Golden (from the software model that mirrors the RTL) ──
    golden_pairs = sw_topk_unified_pairs(
        [list(zip(chunk0_bits, chunk0_idxs)),
         list(zip(chunk1_bits, chunk1_idxs))],
        K=K, P=P, K_MAX=K_MAX, fmt=fmt,
    )
    # SRAM row 0 holds the top-P (= top-K since K==P here) descending.
    expected_vals = [pair[0] for pair in golden_pairs[:P]]
    expected_idxs = [pair[1] for pair in golden_pairs[:P]]

    expected_topk_vals = pack_lanes(expected_vals, val_w)
    expected_topk_idxs = pack_lanes(expected_idxs, idx_w)

    # ── Packed input buses ──
    chunk0_vals_bus = pack_lanes(chunk0_bits, val_w)
    chunk0_idxs_bus = pack_lanes(chunk0_idxs, idx_w)
    chunk1_vals_bus = pack_lanes(chunk1_bits, val_w)
    chunk1_idxs_bus = pack_lanes(chunk1_idxs, idx_w)

    tb.clock("clk")
    tb.reset("rst", cycles_asserted=2, cycles_deasserted=1)
    tb.timeout(int(p["timeout"]))

    # Hold session-stable inputs from cycle 0.
    tb.drive("fmt_sel", fmt_sel_value)
    tb.drive("k_in", K)
    tb.drive("topk_drain_addr", 0)

    # ── cycle 0 : fire chunk 0 ──
    tb.drive("chunk_vals", chunk0_vals_bus)
    tb.drive("chunk_idxs", chunk0_idxs_bus)
    tb.drive("valid_in", 1)

    # Engine is busy for 11 cycles (SORT 3 + PRE 1 + HP 6 + POST 1).
    # ready_out becomes 1 again at cy 12 → fire chunk 1 there.
    tb.next()                                     # → cy 1
    tb.drive("valid_in", 0)
    tb.drive("chunk_vals", 0)
    tb.drive("chunk_idxs", 0)
    for _ in range(2, 12):
        tb.next()
    # ── cycle 12 : fire chunk 1 ──
    tb.drive("chunk_vals", chunk1_vals_bus)
    tb.drive("chunk_idxs", chunk1_idxs_bus)
    tb.drive("valid_in", 1)

    tb.next()                                     # → cy 13
    tb.drive("valid_in", 0)
    tb.drive("chunk_vals", 0)
    tb.drive("chunk_idxs", 0)
    # Walk forward to give chunk 1 the full 11-cycle round-trip plus 6 cycles
    # of SRAM read-port settling (raddr→raddr_d→sram_rdata→output mux).
    for _ in range(14, 31):
        tb.next()

    # ── cycle 30 : final state, sticky running_valid + drained outputs ──
    tb.expect("running_valid", 1, msg="vseen must be sticky after first chunk absorbed")
    tb.expect("ready_out", 1, msg="ready must be 1 once both chunks have settled")
    tb.expect("topk_vals", expected_topk_vals,
              msg=f"top-K vals mismatch (expected {expected_vals})")
    tb.expect("topk_idxs", expected_topk_idxs,
              msg=f"top-K idxs mismatch (expected {expected_idxs})")

    tb.finish(at=int(p["finish"]))


# ═════════════════════════════════════════════════════════════════
# Optional: software-only algorithm matrix (--algo)
# ═════════════════════════════════════════════════════════════════

def gen_random_chunk(N: int, fmt, rng: random.Random,
                     include_specials: bool = False) -> list[int]:
    """Generate ``N`` random bit patterns for ``fmt``.

    Special-value injection (``include_specials=True``) injects NaN / ±inf
    only when the format actually has those encodings; for fp4_e2m1 (no
    NaN, no inf) and fp8_e4m3 (no inf) we substitute representative finite
    edge cases instead (``neg_max_finite_bits`` / ``max_finite_pos_bits``).
    For fp4_e2m1 every bit pattern is a valid finite value, so the
    "uniform float" path falls through to the generic ``float_to_bits``
    helper which clamps via the saturation logic in ``tool.py``.
    """
    name = fmt.name
    out: list[int] = []
    sample_pool: list[float] | None = None
    sample_range: float = 1e3
    if name == "fp4_e2m1":
        sample_pool = [-6, -4, -3, -2, -1.5, -1, -0.5, 0,
                        0.5, 1, 1.5, 2, 3, 4, 6]
    elif name == "fp8_e4m3":
        # fp8_e4m3 max-finite ≈ 448; sample within ±256 to keep most values exact.
        sample_range = 256.0
    for i in range(N):
        if include_specials and i % 47 == 0:
            kind = i // 47 % 5
            if kind == 0 and fmt.has_nan:
                out.append((fmt.exp_all_ones << fmt.man_w) | (i & ((1 << fmt.man_w) - 1) | 1))
                continue
            if kind == 1 and fmt.has_inf:
                out.append((fmt.exp_all_ones << fmt.man_w))
                continue
            if kind == 2 and fmt.has_inf:
                out.append((1 << fmt.sign_bit) | (fmt.exp_all_ones << fmt.man_w))
                continue
            if kind == 3:
                out.append(((i & ((1 << fmt.man_w) - 1)) | 1))
                continue
            if kind == 4:
                out.append(fmt.neg_max_finite_bits if i % 2 else fmt.max_finite_pos_bits)
                continue
        if sample_pool is not None:
            out.append(float_to_bits(rng.choice(sample_pool), fmt))
        else:
            out.append(float_to_bits(rng.uniform(-sample_range, sample_range), fmt))
    return out


def golden_topk_keys(all_bits: list[int], fmt, K: int) -> list[int]:
    keys = [fp_to_unsigned_key(b, fmt) for b in all_bits]
    order = sorted(range(len(keys)), key=lambda i: keys[i], reverse=True)
    return [keys[i] for i in order[:K]]


def run_one_case(*, fmt_name: str, K: int, P: int, K_MAX: int, N: int,
                 include_specials: bool, seed: int) -> bool:
    fmt = fmt_of(fmt_name)
    rng = random.Random(seed)
    if N % P != 0 or K > K_MAX:
        return True
    all_bits = gen_random_chunk(N, fmt, rng, include_specials=include_specials)
    chunks_bits = [all_bits[i:i + P] for i in range(0, N, P)]
    chunks_keys = [[fp_to_unsigned_key(b, fmt) for b in ch] for ch in chunks_bits]
    neg_inf_key = fp_to_unsigned_key(fmt.neg_inf_bits, fmt)
    sw_top = sw_topk_unified(chunks_keys, K, P, K_MAX, neg_inf_key)
    gold_top = golden_topk_keys(all_bits, fmt, K)
    if sw_top != gold_top:
        print(f"  [FAIL] fmt={fmt_name} K={K} P={P} N={N} "
              f"specials={include_specials} seed={seed}")
        return False
    return True


def run_matrix() -> int:
    P, K_MAX = 256, 4096
    K_VALUES = [1, 7, 8, 256, 257, 1024, 4096]
    N_VALUES = [256, 4096, 16384, 65536]
    fmts = ["bf16", "fp16", "fp32", "fp8_e4m3", "fp4_e2m1"]
    n_pass = n_fail = 0
    for fmt_name in fmts:
        print(f"\n──── fmt = {fmt_name} ────")
        for K in K_VALUES:
            for N in N_VALUES:
                if N < K:
                    continue
                ok = run_one_case(fmt_name=fmt_name, K=K, P=P, K_MAX=K_MAX, N=N,
                                  include_specials=False,
                                  seed=0xC0FFEE + K * 31 + N)
                tag = f"K={K:>4d} N={N:>7d}"
                print(f"  [{ 'pass' if ok else 'FAIL'}] {tag}")
                n_pass += 1 if ok else 0
                n_fail += 0 if ok else 1
                if N == 4096:
                    ok2 = run_one_case(fmt_name=fmt_name, K=K, P=P, K_MAX=K_MAX, N=N,
                                       include_specials=True,
                                       seed=0xBADC0DE + K * 31 + N)
                    n_pass += 1 if ok2 else 0
                    n_fail += 0 if ok2 else 1
    print(f"\n──── Summary: {n_pass} passed, {n_fail} failed ────")
    return n_fail


def run_edge_cases() -> int:
    fails = 0
    P, K_MAX = 256, 4096
    for fmt_name in ["bf16", "fp16", "fp32", "fp8_e4m3", "fp4_e2m1"]:
        fmt = fmt_of(fmt_name)
        neg_inf_key = fp_to_unsigned_key(fmt.neg_inf_bits, fmt)
        scenarios = [
            ("all_equal_one", lambda N: [float_to_bits(1.0, fmt)] * N),
            ("strictly_inc",  lambda N: [float_to_bits(float(i % 8) * 0.5, fmt) for i in range(N)]),
            ("strictly_dec",  lambda N: [float_to_bits(float((N - i) % 8) * 0.5, fmt) for i in range(N)]),
            ("mixed_signs",   lambda N: [float_to_bits(((-1) ** i) * ((i % 7) + 1) * 0.25, fmt) for i in range(N)]),
            ("neg_max_finite", lambda N: [fmt.neg_max_finite_bits] * N),
            ("max_pos_finite", lambda N: [fmt.max_finite_pos_bits] * N),
        ]
        if fmt.has_nan:
            scenarios.append(
                ("all_nan", lambda N: [(fmt.exp_all_ones << fmt.man_w)
                                        | (i % max(1, (1 << fmt.man_w) - 1) + 1) for i in range(N)])
            )
        if fmt.has_inf:
            scenarios.append(
                ("all_neg_inf",
                 lambda N: [(1 << fmt.sign_bit) | (fmt.exp_all_ones << fmt.man_w)] * N)
            )
            scenarios.append(
                ("all_pos_inf",
                 lambda N: [(fmt.exp_all_ones << fmt.man_w)] * N)
            )
        for desc, gen in scenarios:
            for K in [1, 7, 8, 256, 257, 1024]:
                N = 1024
                if N % P != 0 or K > K_MAX:
                    continue
                bits = gen(N)
                chunks = [bits[i:i + P] for i in range(0, N, P)]
                chunk_keys = [[fp_to_unsigned_key(b, fmt) for b in ch] for ch in chunks]
                sw_top = sw_topk_unified(chunk_keys, K, P, K_MAX, neg_inf_key)
                gold_top = golden_topk_keys(bits, fmt, K)
                if sw_top != gold_top:
                    print(f"  [FAIL edge] {fmt_name:10s} {desc:>14s} K={K:>4d} N={N}")
                    fails += 1
                else:
                    print(f"  [pass edge] {fmt_name:10s} {desc:>14s} K={K:>4d} N={N}")
    return fails


# ═════════════════════════════════════════════════════════════════
# CLI: emit MLIR for the testbench top (counter pattern), or run --algo
# ═════════════════════════════════════════════════════════════════

if __name__ == "__main__":
    if "--algo" in sys.argv:
        print("══ Top-K algorithm software-model verification ══")
        n_fail = run_matrix() + run_edge_cases()
        sys.exit(0 if n_fail == 0 else 1)
    print(compile_cycle_aware(build, name="tb_topk_top", **DEFAULT_PARAMS).emit_mlir())
