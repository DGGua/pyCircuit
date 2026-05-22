"""Shared smoke-tb stimulus helpers.

Used by `tb_topk_histogram.py` and `tb_topk_histogram_nightly.py` so the
expected-value golden generation lives in one place.

Cycle plan (independent of input data, computed from the FSM in
`topk_histogram.py`):

    cycle 0           : drive in_req=1, cfg_topk
    cycle 1..8        : drive in_data beat 0..7 (in_req=0)
    cycle 9..16       : HIST round 0
    cycle 17          : CUMSUM round 0
    cycle 18..25      : MASK round 0
    cycle 26..33      : HIST round 1
    cycle 34          : CUMSUM round 1
    cycle 35..42      : MASK round 1
    cycle 43..50      : HIST round 2
    cycle 51          : CUMSUM round 2
    cycle 52..59      : MASK round 2
    cycle 60..67      : HIST round 3
    cycle 68          : CUMSUM round 3   (eq_keep latched into eq_remain)
    cycle 69..(...)   : FILTER  (single pass, 1..8 cy, exits when wptr==K)
    cycle X           : WAIT_OUT, out_req=1
    cycle X+1..X+8    : DRAIN (out_value, out_index_data per beat)

For deterministic timing we use all-equal stimuli where gt_count=0, so
FILTER takes exactly ceil(K / LANE_NUM) cycles (capped at BURST_LEN).
"""
from __future__ import annotations

import sys
from pathlib import Path

_THIS_DIR = Path(__file__).resolve().parent
_ROOT_DIR = _THIS_DIR.parent
for _p in (_THIS_DIR, _ROOT_DIR):
    sp = str(_p)
    if sp not in sys.path:
        sys.path.insert(0, sp)

from dataclasses import dataclass  # noqa: E402
from typing import List  # noqa: E402

from tool import float_to_fp32_bits, pack_lanes, split_into_beats  # noqa: E402
from topk_histogram_model import SimResult, simulate_histogram_python  # noqa: E402


# Cycle layout constants (must match topk_histogram.py FSM)
LOAD_START_CY = 1                 # first LOAD cycle (in_req at cycle 0)
LOAD_END_CY = LOAD_START_CY + 8 - 1   # 8 cycles → cycles 1..8
RADIX_CY_PER_NONLAST_ROUND = 8 + 1 + 8    # HIST + CUMSUM + MASK
RADIX_CY_LAST_ROUND = 8 + 1                # HIST + CUMSUM (no MASK)
RADIX_TOTAL_CY = 3 * RADIX_CY_PER_NONLAST_ROUND + RADIX_CY_LAST_ROUND  # 60
LANE_NUM = 128
BURST_LEN = 8


@dataclass
class SmokeStimulus:
    """Bundle of test inputs + golden + cycle plan."""

    K: int
    in_beats: List[List[int]]      # 8 × 128 fp32 bit patterns
    beat_buses: List[int]          # 8 × 4096-bit packed in_data values
    golden: SimResult
    filter_cy: int                 # number of FILTER cycles before WAIT_OUT
    wait_out_cy: int               # cycle index of WAIT_OUT (out_req asserted)
    drain_start_cy: int            # cycle index of DRAIN first beat (out_value beat 0)
    finish_cy: int                 # cycle index where tb.finish runs


def gen_alleq_stimulus(*, K: int, value: float = 3.5) -> SmokeStimulus:
    """Build deterministic stimulus where every input element equals ``value``.

    With all-same input the FSM behaviour is fully predictable: gt_count = 0
    (no element is strictly greater than kth_key), so eq_keep = K and the
    single-pass FILTER writes 128 elements per cycle until wptr reaches K.
    Total FILTER cycles = min(ceil(K / LANE_NUM), BURST_LEN).
    """
    if K < 1 or K > 1024:
        raise ValueError(f"gen_alleq_stimulus requires 1 <= K <= 1024 (got {K})")
    val_bits = float_to_fp32_bits(float(value))
    stream = [val_bits] * 1024
    beats = split_into_beats(stream, lanes_per_beat=LANE_NUM)
    beat_buses = [pack_lanes(b, lane_w=32) for b in beats]
    golden = simulate_histogram_python(beats, K=K)

    # Single-pass FILTER: each cycle writes min(LANE_NUM, eq_remain) elements
    # until wptr_next reaches K (exits same cycle, no extra eq_done check).
    filter_cy = min((K + LANE_NUM - 1) // LANE_NUM, BURST_LEN)
    return _assemble_stim(K=K, beats=beats, beat_buses=beat_buses,
                           golden=golden, filter_cy=filter_cy)


def _assemble_stim(
    *, K: int, beats, beat_buses, golden, filter_cy,
) -> SmokeStimulus:
    # FILTER phase end cycle (last in_filter cycle)
    filt_end_cy = LOAD_END_CY + RADIX_TOTAL_CY + filter_cy
    wait_out_cy = filt_end_cy + 1
    drain_start_cy = wait_out_cy + 1
    finish_cy = drain_start_cy + 8 + 4      # tail margin
    return SmokeStimulus(
        K=K, in_beats=beats, beat_buses=beat_buses, golden=golden,
        filter_cy=filter_cy, wait_out_cy=wait_out_cy,
        drain_start_cy=drain_start_cy, finish_cy=finish_cy,
    )


def drive_request_and_load(tb, stim: SmokeStimulus) -> None:
    """Drive in_req at cycle 0, then 8-beat in_data at cycles 1..8.

    Caller is responsible for advancing the tb cycle counter (tb.next).
    """
    # cycle 0: REQ
    tb.drive("cfg_topk", stim.K)
    tb.drive("in_req", 1)
    # leave in_data x; in_data shouldn't matter at cycle 0 since sram_wvalid=0
    tb.drive("in_data", 0)

    # cycles 1..8: 8-beat data, in_req lowered
    for i in range(8):
        tb.next()
        tb.drive("in_req", 0)
        tb.drive("in_data", stim.beat_buses[i])


def wait_until(tb, target_cy: int, current_cy: int) -> int:
    """Advance the tb up to (but not exceeding) ``target_cy``. Returns new cy."""
    while current_cy < target_cy:
        tb.next()
        current_cy += 1
    return current_cy


def expect_output_burst(tb, stim: SmokeStimulus) -> None:
    """Issue expects for out_req + 8 DRAIN beats.

    Caller must have advanced the tb to ``stim.wait_out_cy`` BEFORE calling
    this function. After return, the tb is positioned at the cycle AFTER the
    last DRAIN beat.
    """
    tb.expect("out_req", 1)
    for beat in range(8):
        tb.next()
        tb.expect("out_value",      stim.golden.out_value_beats[beat])
        tb.expect("out_index_data", stim.golden.out_index_beats[beat])
        tb.expect("out_valid_mask", stim.golden.out_valid_mask_beats[beat])
        # out_req should be 0 during drain (only 1 cy of wait_out)
        tb.expect("out_req", 0)
