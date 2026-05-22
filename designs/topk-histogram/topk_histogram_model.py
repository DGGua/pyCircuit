"""Pure-Python software model for the radix-select Top-K accelerator.

Mirrors the phase-accurate cycle table in
[designs/topk-histogram/arch.md](designs/topk-histogram/arch.md) §5.2:

    LOAD       : 8 beats × 128 fp32 → 1024 sortable keys in `data_sram`
    RADIX × 4  : per round, 8-cy histogram + 4-cy cumsum + mask update
    KTH_COMPOSE: kth_key = (tb0<<24)|(tb1<<16)|(tb2<<8)|tb3   (MSB-first)
    FILTER_GT  : 8 beats — pick all elements with key > kth_key
    FILTER_EQ  : 0..8 beats — pad with equals to hit K (lane-truncated)
    DRAIN      : 8 beats of packed (value, elem_idx) + out_valid_mask

The model returns a :class:`SimResult` dataclass with both the intermediate
state (target_bin per round, kth_key, gt_count, total_count) and the final
8-beat output bus contents — enough to back both Layer A (algorithm check)
and Layer C (testbench expect replay).

Ties / K=0 / NaN follow the hard contracts from arch.md §10:

    * elem_idx = beat * LANE_NUM + lane     (10-bit for N=1024, zero-extended)
    * Ties selected in (beat ascending, lane ascending) scan order
    * cfg_topk = 0 → hardware clamps to 1
    * NaN: "is NaN" preserved, payload may change (key_to_fp folds canonical
      qNaN to 0xFFFFFFFF and back to a +NaN)
"""
from __future__ import annotations

from dataclasses import dataclass, field
from typing import List, Optional, Sequence

from fp_key import fp_to_key_py, key_to_fp_py
from tool import is_fp32_nan_bits


# ═════════════════════════════════════════════════════════════════
# Data classes
# ═════════════════════════════════════════════════════════════════

@dataclass
class SimResult:
    """Phase-accurate trace of one Top-K invocation."""

    # Inputs / config (echoed for debug)
    N: int
    LANE_NUM: int
    K_effective: int

    # data_sram contents after LOAD: 1024 sortable keys, addr-major
    sram_keys: List[int] = field(default_factory=list)

    # RADIX intermediate state, per round 0..3
    hist: List[List[int]] = field(default_factory=list)         # [round][256] counts
    cumsum: List[List[int]] = field(default_factory=list)       # [round][256]
    target_bin: List[int] = field(default_factory=list)         # [round]
    prev_cum: List[int] = field(default_factory=list)           # [round]
    bottomK_per_round: List[int] = field(default_factory=list)  # [round]

    # Composed kth_key (sortable key form), 32-bit
    kth_key: int = 0

    # Filter pass results
    gt_count: int = 0
    eq_count: int = 0
    total_count: int = 0

    # Compact output list of (fp32_bits, elem_idx) pairs in scan order
    output_pairs: List[tuple[int, int]] = field(default_factory=list)

    # 8-beat output buses (each 4096-bit) + mask per beat (LANE_NUM bits)
    out_value_beats: List[int] = field(default_factory=list)
    out_index_beats: List[int] = field(default_factory=list)
    out_valid_mask_beats: List[int] = field(default_factory=list)


# ═════════════════════════════════════════════════════════════════
# Core simulator
# ═════════════════════════════════════════════════════════════════

_VAL_W = 32
_LANE_W_OUT = 32          # both value and index lanes are 32-bit on the output bus
_RADIX_BITS = 8
_NUM_BINS = 1 << _RADIX_BITS    # 256
_NUM_ROUNDS = 32 // _RADIX_BITS  # 4


def _byte_of_key(key: int, round_idx: int) -> int:
    """Round 0 → MSB byte (key[31:24]); round 3 → LSB byte (key[7:0])."""
    shift = (_NUM_ROUNDS - 1 - round_idx) * _RADIX_BITS    # 24, 16, 8, 0
    return (key >> shift) & 0xFF


def _pack_lanes(lanes: Sequence[int], lane_w: int) -> int:
    """Pack lanes lane-0-in-LSB (mirrors RTL `_pack_lanes_lsb_first`)."""
    mask = (1 << lane_w) - 1
    bus = 0
    for i, v in enumerate(lanes):
        bus |= (int(v) & mask) << (i * lane_w)
    return bus


def simulate_histogram_python(
    in_beats: Sequence[Sequence[int]],
    *,
    K: int,
    N: int = 1024,
    LANE_NUM: int = 128,
) -> SimResult:
    """Run the full radix-select Top-K pipeline on ``in_beats`` (8×128 fp32).

    Returns :class:`SimResult` with phase-by-phase state plus the final
    8-beat output bus contents (`out_value_beats`, `out_index_beats`,
    `out_valid_mask_beats`).
    """
    BURST = len(in_beats)
    if BURST * LANE_NUM != N:
        raise ValueError(
            f"in_beats shape mismatch: {BURST} beats × {LANE_NUM} lanes != N={N}"
        )
    for b, beat in enumerate(in_beats):
        if len(beat) != LANE_NUM:
            raise ValueError(f"beat {b} has {len(beat)} lanes, expected {LANE_NUM}")

    # K=0 clamp (arch §3.1 / §10)
    K_eff = K if K > 0 else 1
    if K_eff > N:
        raise ValueError(f"K={K} exceeds N={N}")

    # === LOAD: fp32 → sortable key ===
    sram_keys: List[int] = []
    for beat in in_beats:
        for lane_bits in beat:
            sram_keys.append(fp_to_key_py(int(lane_bits) & 0xFFFFFFFF))
    assert len(sram_keys) == N

    res = SimResult(N=N, LANE_NUM=LANE_NUM, K_effective=K_eff, sram_keys=sram_keys)

    # === RADIX × 4 ===
    mask = [True] * N
    bottomK = N - K_eff + 1     # smallest j s.t. cumsum[j] ≥ bottomK is target bin

    for r in range(_NUM_ROUNDS):
        # Histogram over masked elements
        hist = [0] * _NUM_BINS
        for i in range(N):
            if mask[i]:
                hist[_byte_of_key(sram_keys[i], r)] += 1

        # Cumulative sum (inclusive)
        cumsum: List[int] = []
        run = 0
        for c in hist:
            run += c
            cumsum.append(run)

        # Priority encode: smallest j s.t. cumsum[j] ≥ bottomK
        target_bin = -1
        for j in range(_NUM_BINS):
            if cumsum[j] >= bottomK:
                target_bin = j
                break
        if target_bin < 0:
            # Should never happen: sum(hist) = popcount(mask) ≥ bottomK by construction
            raise AssertionError(
                f"radix round {r}: no bin satisfies cumsum ≥ bottomK={bottomK}; "
                f"sum(hist)={sum(hist)}"
            )
        prev_cum = cumsum[target_bin - 1] if target_bin > 0 else 0

        res.hist.append(hist)
        res.cumsum.append(cumsum)
        res.target_bin.append(target_bin)
        res.prev_cum.append(prev_cum)
        res.bottomK_per_round.append(bottomK)

        # Tighten mask for next round + update bottomK
        if r < _NUM_ROUNDS - 1:
            for i in range(N):
                if mask[i] and _byte_of_key(sram_keys[i], r) != target_bin:
                    mask[i] = False
            bottomK = bottomK - prev_cum

    # === KTH_COMPOSE (MSB-first) ===
    kth_key = 0
    for r in range(_NUM_ROUNDS):
        kth_key = (kth_key << _RADIX_BITS) | (res.target_bin[r] & 0xFF)
    res.kth_key = kth_key

    # === FILTER passes (scan-order: addr ascending, lane ascending) ===
    output_pairs: List[tuple[int, int]] = []

    # GT pass: take everything strictly greater than kth_key
    for i in range(N):
        if sram_keys[i] > kth_key:
            output_pairs.append((key_to_fp_py(sram_keys[i]), i))
    res.gt_count = len(output_pairs)

    # EQ pass: top up with == kth_key until we hit K_eff
    if res.gt_count < K_eff:
        remaining = K_eff - res.gt_count
        for i in range(N):
            if remaining == 0:
                break
            if sram_keys[i] == kth_key:
                output_pairs.append((key_to_fp_py(sram_keys[i]), i))
                remaining -= 1
        res.eq_count = len(output_pairs) - res.gt_count
    else:
        res.eq_count = 0
    res.total_count = len(output_pairs)
    assert res.total_count <= K_eff, (
        f"filter overshoot: got {res.total_count} > K_eff={K_eff}"
    )

    res.output_pairs = output_pairs

    # === Output bus packing (8 beats × 128 lanes × 32-bit) ===
    pad = LANE_NUM * BURST - len(output_pairs)
    value_lanes = [v for v, _ in output_pairs] + [0] * pad
    index_lanes = [i for _, i in output_pairs] + [0] * pad
    for b in range(BURST):
        val_beat = value_lanes[b * LANE_NUM : (b + 1) * LANE_NUM]
        idx_beat = index_lanes[b * LANE_NUM : (b + 1) * LANE_NUM]
        res.out_value_beats.append(_pack_lanes(val_beat, lane_w=_LANE_W_OUT))
        res.out_index_beats.append(_pack_lanes(idx_beat, lane_w=_LANE_W_OUT))
        res.out_valid_mask_beats.append(_mask_for_beat(b, res.total_count, LANE_NUM))

    return res


def _mask_for_beat(beat: int, total_count: int, LANE_NUM: int) -> int:
    """Per-beat out_valid_mask (arch §4.2.2 formula)."""
    if (beat + 1) * LANE_NUM <= total_count:
        return (1 << LANE_NUM) - 1
    if beat * LANE_NUM >= total_count:
        return 0
    used = total_count - beat * LANE_NUM
    return (1 << used) - 1


# ═════════════════════════════════════════════════════════════════
# Self-test (Layer A)
# ═════════════════════════════════════════════════════════════════

def _fp32_bits_of(f: float) -> int:
    import struct
    return struct.unpack(">I", struct.pack(">f", f))[0]


def _argpartition_topk(bits: Sequence[int], K: int) -> set[int]:
    """Reference top-K set (no order constraint, NaN-aware).

    Compares using ``fp_to_key_py`` (the same monotone unsigned key as the
    model) so we agree on -0 < +0, NaN > +Inf, etc.
    """
    keys = [fp_to_key_py(b) for b in bits]
    sorted_indices = sorted(range(len(bits)), key=lambda i: keys[i], reverse=True)
    return set(sorted_indices[:K])


def _check_topk_set(bits: Sequence[int], K: int, *, N: int = 1024, LANE_NUM: int = 128) -> None:
    """Run the model on ``bits`` and assert its picked indices match argpartition."""
    beats = [list(bits[b * LANE_NUM : (b + 1) * LANE_NUM]) for b in range(N // LANE_NUM)]
    res = simulate_histogram_python(beats, K=K, N=N, LANE_NUM=LANE_NUM)
    got = {idx for _, idx in res.output_pairs}
    K_eff = K if K > 0 else 1
    assert len(got) == K_eff, f"K={K}: model returned {len(got)} elements, expected {K_eff}"

    # The exact set we pick may differ from argpartition's *only* when there
    # are ties at the K-th boundary. To be robust, check that:
    #   1) all GT elements (key > kth) are in the picked set
    #   2) #(EQ picked) + #(GT) == K_eff
    #   3) every picked element has key >= kth_key
    keys = [fp_to_key_py(b) for b in bits]
    kth = res.kth_key
    for _, idx in res.output_pairs:
        assert keys[idx] >= kth, f"picked elem 0x{bits[idx]:08x} below kth=0x{kth:08x}"
    gt_set = {i for i in range(N) if keys[i] > kth}
    assert gt_set <= got, "every strictly-greater-than-kth element must be picked"
    # And the kth_key is the K-th largest by rank
    ranks = sorted(keys, reverse=True)
    assert ranks[K_eff - 1] == kth, (
        f"kth_key=0x{kth:08x} != true K-th largest key=0x{ranks[K_eff - 1]:08x}"
    )


def _selftest() -> None:
    from tool import gen_random_fp32

    # 1) Random data, several K values
    for seed in range(5):
        bits = gen_random_fp32(1024, seed=seed)
        for K in (1, 2, 128, 200, 900, 1023, 1024):
            _check_topk_set(bits, K)

    # 2) K=0 should be clamped to 1
    bits = gen_random_fp32(1024, seed=0)
    res = simulate_histogram_python(
        [list(bits[b * 128 : (b + 1) * 128]) for b in range(8)],
        K=0,
    )
    assert res.K_effective == 1
    assert res.total_count == 1

    # 3) All-same value: every element ties at kth_key
    same = [_fp32_bits_of(3.5)] * 1024
    res = simulate_histogram_python(
        [list(same[b * 128 : (b + 1) * 128]) for b in range(8)],
        K=64,
    )
    assert res.total_count == 64
    assert res.gt_count == 0           # nothing strictly greater than the only value
    assert res.eq_count == 64
    # Picked indices should be the first 64 (scan order)
    assert [idx for _, idx in res.output_pairs] == list(range(64))

    # 4) All NaN: should still produce K elements with NaN values
    nan_bits = 0x7FC00000
    nan_stream = [nan_bits] * 1024
    res = simulate_histogram_python(
        [list(nan_stream[b * 128 : (b + 1) * 128]) for b in range(8)],
        K=8,
    )
    assert res.total_count == 8
    for v, _ in res.output_pairs:
        assert is_fp32_nan_bits(v), f"expected NaN, got 0x{v:08x}"

    # 5) Output bus mask shape: K=900 → 7 full beats + last beat low 4
    res = simulate_histogram_python(
        [list(gen_random_fp32(1024, seed=42)[b * 128 : (b + 1) * 128]) for b in range(8)],
        K=900,
    )
    all_ones = (1 << 128) - 1
    for b in range(7):
        assert res.out_valid_mask_beats[b] == all_ones, (
            f"beat {b} should be full mask, got 0x{res.out_valid_mask_beats[b]:x}"
        )
    assert res.out_valid_mask_beats[7] == (1 << 4) - 1, (
        f"beat 7 should be low-4 mask, got 0x{res.out_valid_mask_beats[7]:x}"
    )

    # 6) K=200 → beat0 full, beat1 low 72, beat2..7 empty
    res = simulate_histogram_python(
        [list(gen_random_fp32(1024, seed=1)[b * 128 : (b + 1) * 128]) for b in range(8)],
        K=200,
    )
    assert res.out_valid_mask_beats[0] == all_ones
    assert res.out_valid_mask_beats[1] == (1 << 72) - 1
    for b in range(2, 8):
        assert res.out_valid_mask_beats[b] == 0

    # 7) Mixed ±0 / ±Inf / NaN / normals: just check we don't crash and the
    #    picked K elements all have key ≥ kth_key.
    mix = (
        [0x00000000, 0x80000000, 0x7F800000, 0xFF800000, 0x7FC00000, 0x3F800000]
        * (1024 // 6) + [0x00000000] * (1024 % 6)
    )
    _check_topk_set(mix, K=128)

    # 8) Output pairs are in (addr, lane) scan order
    bits = gen_random_fp32(1024, seed=7)
    res = simulate_histogram_python(
        [list(bits[b * 128 : (b + 1) * 128]) for b in range(8)],
        K=300,
    )
    idxs = [idx for _, idx in res.output_pairs]
    # Within GT and within EQ separately, indices should be ascending
    assert idxs[: res.gt_count] == sorted(idxs[: res.gt_count])
    assert idxs[res.gt_count :] == sorted(idxs[res.gt_count :])

    # 9) Single-pass FILTER hardware claim (see arch.md §4.2.9):
    #    eq_keep = hist[3][target_bin[3]] - bottomK_4 + 1
    #    where bottomK_4 = bottomK_per_round[3] - prev_cum[3].
    #    gt_count = K - eq_keep   (when eq_keep > 0, else GT alone covers K).
    #
    # Exercise this formula across random + corner stimuli; failure would
    # mean the RTL's eq_remain init is wrong → over/under-writing in FILTER.
    def _check_eq_keep_formula(bits_in, K):
        rr = simulate_histogram_python(
            [list(bits_in[b * 128 : (b + 1) * 128]) for b in range(8)],
            K=K,
        )
        tb3 = rr.target_bin[3]
        hist3_tb3 = rr.hist[3][tb3]
        bottomK_4 = rr.bottomK_per_round[3] - rr.prev_cum[3]
        eq_keep_pred = hist3_tb3 - bottomK_4 + 1
        K_eff = K if K > 0 else 1

        # eq_keep predicted must equal what the model actually wrote in EQ pass
        # (or 0 if GT alone reached K).
        if rr.gt_count >= K_eff:
            assert rr.eq_count == 0
            # eq_keep formula can give a non-positive number when GT >= K;
            # the hardware would in that case never enter FILTER's EQ-taking
            # branch (since eq_remain ≤ 0 means no lane passes the gating).
        else:
            assert eq_keep_pred == rr.eq_count, (
                f"K={K}: predicted eq_keep={eq_keep_pred} != actual eq_count={rr.eq_count}"
            )
            assert rr.gt_count == K_eff - eq_keep_pred, (
                f"K={K}: gt={rr.gt_count} != K - eq_keep_pred = {K_eff - eq_keep_pred}"
            )

    # Random data, wide K coverage
    for seed in (0, 1, 2, 7, 42):
        bits = gen_random_fp32(1024, seed=seed)
        for K in (1, 2, 4, 128, 200, 300, 900, 1023, 1024):
            _check_eq_keep_formula(bits, K)

    # Mixed special-value: all the corner-case fp32 patterns share one bin
    mix = (
        [0x00000000, 0x80000000, 0x7F800000, 0xFF800000, 0x7FC00000, 0x3F800000]
        * (1024 // 6) + [0x00000000] * (1024 % 6)
    )
    for K in (1, 4, 128, 512, 1024):
        _check_eq_keep_formula(mix, K)

    # All-equal: max EQ case (gt=0, eq_keep=K)
    _check_eq_keep_formula([_fp32_bits_of(3.5)] * 1024, K=4)
    _check_eq_keep_formula([_fp32_bits_of(3.5)] * 1024, K=900)
    _check_eq_keep_formula([_fp32_bits_of(3.5)] * 1024, K=1024)

    print("topk_histogram_model.py: selftest OK")


if __name__ == "__main__":
    _selftest()
