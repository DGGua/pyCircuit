"""Per-fmt smoke testbench library (shares the unified DUT geometry).

Smoke geometry uses ``DEFAULT_PARAMS`` (P=4, K_MAX=16, idx_w=4) and runs
two chunks at K=4 (rows_used=1) so the cycle plan is short:

    cy 0    : fire chunk 0 (ready_out=1, fmt_sel = per-fmt code)
    cy 1..11: engine busy (SORT 3 + PRE 1 + HP 6 + POST 1)
    cy 12   : fire chunk 1
    cy 13..23 : engine busy
    cy 24   : IDLE (mem[0] = final top-K)
    cy 30   : sample (raddr→raddr_d→sram_rdata settles in 2 cy after IDLE).

Each ``tb_topk_smoke_<FMT>.py`` driver dispatches to ``drive_smoke_tb``
with its own ``fmt_name`` so each binary covers its own datapath.
"""
from __future__ import annotations

import math
import sys
from pathlib import Path

from pycircuit import CycleAwareTb, Tb

_THIS_DIR = Path(__file__).resolve().parent
if str(_THIS_DIR) not in sys.path:
    sys.path.insert(0, str(_THIS_DIR))

from topk_config import (  # noqa: E402
    DEFAULT_PARAMS,
    TB_PRESETS,
    FMT_BF16, FMT_FP16, FMT_FP32, FMT_FP8_E4M3, FMT_FP4_E2M1,
    fmt_of,
)
from tb_topk import (  # noqa: E402
    pack_lanes,
    sw_topk_unified_pairs,
)
from tool import float_to_bits  # noqa: E402


_FMT_SEL: dict[str, int] = {
    "bf16":     FMT_BF16,
    "fp16":     FMT_FP16,
    "fp32":     FMT_FP32,
    "fp8_e4m3": FMT_FP8_E4M3,
    "fp4_e2m1": FMT_FP4_E2M1,
}


# Per-fmt smoke chunks. Values stay within each format's exact range so
# the bit pattern survives the ``float_to_bits`` round-trip exactly.
SMOKE_CHUNKS: dict[str, tuple[list[float], list[float]]] = {
    # bf16 / fp16 / fp32 cover full IEEE behaviour with mixed signs and
    # tie-cases.
    "bf16":     ([3.0, 1.0, 4.0, 7.0], [5.0, 9.0, 2.0, 6.0]),
    "fp16":     ([3.0, 1.0, 4.0, 7.0], [5.0, 9.0, 2.0, 6.0]),
    "fp32":     ([3.0, 1.0, 4.0, 7.0], [5.0, 9.0, 2.0, 6.0]),
    # fp8_e4m3 max-finite ≈ 448; pick small integers that survive exactly.
    "fp8_e4m3": ([3.0, 1.0, 4.0, 7.0], [5.0, 9.0, 2.0, 6.0]),
    # fp4_e2m1 has 16 values total: ±{0, 0.5, 1, 1.5, 2, 3, 4, 6}.
    "fp4_e2m1": ([1.5, -2.0, 0.5, 4.0], [3.0, 6.0, -0.5, 2.0]),
}


def drive_smoke_tb(t: Tb, *, fmt_name: str) -> None:
    """Drive the 2-chunk smoke workload on ``t`` for the given fmt."""
    if fmt_name not in _FMT_SEL:
        raise ValueError(f"unknown fmt {fmt_name!r}")

    P     = int(DEFAULT_PARAMS["P"])
    K_MAX = int(DEFAULT_PARAMS["K_MAX"])
    idx_w = int(DEFAULT_PARAMS["idx_w"])
    val_w = 32
    K     = 4
    fmt   = fmt_of(fmt_name)
    fmt_sel_value = _FMT_SEL[fmt_name]

    log2_P = int(math.log2(P))
    L_S = log2_P * (log2_P + 1) // 2          # 3 for P=4
    L_M = 2 * (log2_P + 1)                    # 6 for P=4
    cy_per_chunk = L_S + 1 + (L_M + 1)        # rows_used=1 -> 11
    fire_period = cy_per_chunk + 1            # IDLE→fire spacing -> 12
    sample_cy = fire_period * 2 + 6           # 2 chunks + raddr settle
    p = TB_PRESETS["smoke"]
    finish_cy = max(sample_cy + 8, int(p["finish"]))
    timeout_cy = max(int(p["timeout"]), finish_cy + 16)

    chunk0_vals, chunk1_vals = SMOKE_CHUNKS[fmt_name]
    chunk0_idxs = [0, 1, 2, 3]
    chunk1_idxs = [4, 5, 6, 7]

    chunk0_bits = [float_to_bits(v, fmt) for v in chunk0_vals]
    chunk1_bits = [float_to_bits(v, fmt) for v in chunk1_vals]

    # Golden via the bit-exact software model.
    golden_pairs = sw_topk_unified_pairs(
        [list(zip(chunk0_bits, chunk0_idxs)),
         list(zip(chunk1_bits, chunk1_idxs))],
        K=K, P=P, K_MAX=K_MAX, fmt=fmt,
    )
    expected_vals = [pair[0] for pair in golden_pairs[:P]]
    expected_idxs = [pair[1] for pair in golden_pairs[:P]]
    expected_topk_vals = pack_lanes(expected_vals, val_w)
    expected_topk_idxs = pack_lanes(expected_idxs, idx_w)

    chunk0_vals_bus = pack_lanes(chunk0_bits, val_w)
    chunk0_idxs_bus = pack_lanes(chunk0_idxs, idx_w)
    chunk1_vals_bus = pack_lanes(chunk1_bits, val_w)
    chunk1_idxs_bus = pack_lanes(chunk1_idxs, idx_w)

    tb = CycleAwareTb(t)
    tb.clock("clk")
    tb.reset("rst", cycles_asserted=2, cycles_deasserted=1)
    tb.timeout(timeout_cy)

    tb.drive("fmt_sel", fmt_sel_value)
    tb.drive("k_in", K)
    tb.drive("topk_drain_addr", 0)

    # cy 0: fire chunk 0
    tb.drive("chunk_vals", chunk0_vals_bus)
    tb.drive("chunk_idxs", chunk0_idxs_bus)
    tb.drive("valid_in", 1)
    tb.next()
    tb.drive("valid_in", 0)
    tb.drive("chunk_vals", 0)
    tb.drive("chunk_idxs", 0)
    cur_cy = 1
    while cur_cy < fire_period:
        tb.next()
        cur_cy += 1

    # cy fire_period: fire chunk 1
    tb.drive("chunk_vals", chunk1_vals_bus)
    tb.drive("chunk_idxs", chunk1_idxs_bus)
    tb.drive("valid_in", 1)
    tb.next()
    cur_cy += 1
    tb.drive("valid_in", 0)
    tb.drive("chunk_vals", 0)
    tb.drive("chunk_idxs", 0)

    while cur_cy < sample_cy:
        tb.next()
        cur_cy += 1

    tb.expect("running_valid", 1,
              msg=f"[{fmt_name}] running_valid sticky after first chunk absorbed")
    tb.expect("ready_out", 1,
              msg=f"[{fmt_name}] ready must be 1 once both chunks have settled")
    tb.expect("topk_vals", expected_topk_vals,
              msg=f"[{fmt_name}] top-K vals mismatch (expected {expected_vals})")
    tb.expect("topk_idxs", expected_topk_idxs,
              msg=f"[{fmt_name}] top-K idxs mismatch (expected {expected_idxs})")

    tb.finish(at=finish_cy)
