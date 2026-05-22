"""Heavy Top-K RTL workload library — production geometry.

The unified engine geometry is::

    P = 256 (sort/merge unit)
    K_MAX = 4096 (SRAM = K_MAX/P = 16 rows)
    idx_w = 13 (covers fp4 worst-case N=8192)

Each chunk takes ``cy_per_chunk = L_S + 1 + (L_M + 1) * rows_used`` cycles
where ``L_S = log2(P)*(log2(P)+1)/2 = 36`` and ``L_M = 2*(log2(P)+1) = 18``.
For the default ``LARGE_TB`` (K=64 → rows_used=1) cy/chunk = 36+1+18+1 = 56.

The harness fires one chunk per ``cy_per_chunk`` cycles (engine is busy
the whole time; ``ready_out=1`` only when in IDLE), so for ``n_chunks``
the last fire happens at cycle ``cy_per_chunk * (n_chunks - 1)`` and the
engine returns to IDLE one fire-period later. We then sample two SRAM rows
to verify the full top-K result against the bit-exact software model
(``sw_topk_unified_pairs``).

Each fmt-specific ``tb_topk_large_<FMT>.py`` driver does::

    @testbench
    def tb(t: Tb) -> None:
        drive_large_tb(t, fmt_name="bf16")  # or fp16 / fp32 / fp8_e4m3 / fp4_e2m1

so that ``pycircuit.cli build`` picks up the driver as ``mod.tb`` and emits
one independent Verilator binary per format.
"""
from __future__ import annotations

import inspect
import math
import random
import sys
from pathlib import Path
from typing import Any, Mapping

from pycircuit import CycleAwareTb, Tb

_THIS_DIR = Path(__file__).resolve().parent
if str(_THIS_DIR) not in sys.path:
    sys.path.insert(0, str(_THIS_DIR))

from topk_config import (  # noqa: E402
    LARGE_PARAMS,
    LARGE_TB,
    FMT_BF16, FMT_FP16, FMT_FP32, FMT_FP8_E4M3, FMT_FP4_E2M1,
    fmt_of,
)
from tb_topk import (  # noqa: E402
    gen_random_chunk,
    pack_lanes,
    sw_topk_unified_pairs,
)
from topk import simulate_engine_python  # noqa: E402


_FMT_SEL: dict[str, int] = {
    "bf16":     FMT_BF16,
    "fp16":     FMT_FP16,
    "fp32":     FMT_FP32,
    "fp8_e4m3": FMT_FP8_E4M3,
    "fp4_e2m1": FMT_FP4_E2M1,
}


def override_build_defaults(build_fn: Any, new_defaults: Mapping[str, Any]) -> Any:
    """Re-export ``build_fn`` with its signature defaults overridden.

    ``pycircuit.cli build`` reads JIT parameter defaults from the decorated
    build function's :func:`inspect.signature`. We can't simply wrap
    ``build`` (the wrapper would lose ``@module`` metadata), so we attach a
    synthetic ``__signature__`` that reports the new defaults while the
    underlying callable / decorators stay intact.
    """
    sig = inspect.signature(build_fn)
    new_params = []
    for p in sig.parameters.values():
        if p.name in new_defaults:
            new_params.append(p.replace(default=new_defaults[p.name]))
        else:
            new_params.append(p)
    build_fn.__signature__ = sig.replace(parameters=new_params)
    return build_fn


def _engine_cycles_per_chunk(P: int, rows_used: int) -> int:
    """SORT (L_S) + PRE (1) + (HP (L_M) + POST (1)) * rows_used."""
    log2_P = int(math.log2(P))
    L_S = log2_P * (log2_P + 1) // 2
    L_M = 2 * (log2_P + 1)
    return L_S + 1 + (L_M + 1) * rows_used


def drive_large_tb(
    t: Tb,
    *,
    fmt_name: str,
    n_chunks_override: int | None = None,
    K_override: int | None = None,
) -> None:
    """Drive the heavy workload for a given fmt onto ``t`` (CycleAwareTb).

    ``n_chunks_override`` shrinks the workload for fast debug iteration,
    ``K_override`` overrides ``LARGE_TB['K']`` so each driver can pick its
    own row footprint (rows_used = ceil(K/P)).
    """
    if fmt_name not in _FMT_SEL:
        raise ValueError(f"unknown fmt {fmt_name!r}; must be one of {list(_FMT_SEL)}")

    P     = int(LARGE_PARAMS["P"])
    K_MAX = int(LARGE_PARAMS["K_MAX"])
    idx_w = int(LARGE_PARAMS["idx_w"])

    K = int(K_override) if K_override is not None else int(LARGE_TB["K"])
    n_chunks = (
        int(n_chunks_override) if n_chunks_override is not None
        else int(LARGE_TB["n_chunks"])
    )
    seed = int(LARGE_TB["seed"])

    rows_used   = (K + P - 1) // P
    if rows_used < 1:
        rows_used = 1
    cy_per_chunk = _engine_cycles_per_chunk(P, rows_used)

    last_fire_cy = cy_per_chunk * (n_chunks - 1)
    last_idle_cy = last_fire_cy + cy_per_chunk           # engine IDLE again
    sample_row0  = last_idle_cy + int(LARGE_TB["sample_extra"])
    finish_cy    = sample_row0 + int(LARGE_TB.get("row1_offset", 2)) \
                                 + int(LARGE_TB["finish_extra"])
    timeout      = finish_cy + int(LARGE_TB["timeout_extra"])

    val_w = 32
    fmt = fmt_of(fmt_name)
    fmt_sel_value = _FMT_SEL[fmt_name]

    # ── Generate workload (per-fmt seeded so each binary differs) ──
    rng = random.Random(seed ^ (hash(fmt_name) & 0xFFFF))
    n_total = P * n_chunks
    all_bits = gen_random_chunk(n_total, fmt, rng, include_specials=False)
    all_idxs = list(range(n_total))

    chunks_bits = [all_bits[i * P : (i + 1) * P] for i in range(n_chunks)]
    chunks_idxs = [all_idxs[i * P : (i + 1) * P] for i in range(n_chunks)]

    # ── Golden via the bit-exact software model ──
    # Use simulate_engine_python to get the full SRAM state row-by-row
    # (each row is exactly P pairs in descending order). Run with the same
    # K as the DUT so rows_used (and the per-row content) match.
    chunks_pairs = [list(zip(cb, ci)) for cb, ci in zip(chunks_bits, chunks_idxs)]
    sram_rows = simulate_engine_python(
        chunks_pairs, K=K, P=P, K_MAX=K_MAX, fmt=fmt,
    )

    row0_pairs = sram_rows[0]
    assert len(row0_pairs) == P, f"sram row 0 has {len(row0_pairs)} pairs, expected {P}"
    row0_vals = [pair[0] for pair in row0_pairs]
    row0_idxs = [pair[1] for pair in row0_pairs]
    row0_vals_bus = pack_lanes(row0_vals, val_w)
    row0_idxs_bus = pack_lanes(row0_idxs, idx_w)

    if rows_used >= 2 and len(sram_rows) >= 2:
        row1_pairs = sram_rows[1]
        row1_vals = [pair[0] for pair in row1_pairs]
        row1_idxs = [pair[1] for pair in row1_pairs]
        row1_vals_bus = pack_lanes(row1_vals, val_w)
        row1_idxs_bus = pack_lanes(row1_idxs, idx_w)
    else:
        row1_vals_bus = None
        row1_idxs_bus = None

    chunk_v_buses = [pack_lanes(cb, val_w) for cb in chunks_bits]
    chunk_i_buses = [pack_lanes(ci, idx_w) for ci in chunks_idxs]

    # ── TB scaffolding ──
    tb = CycleAwareTb(t)
    tb.clock("clk")
    tb.reset("rst", cycles_asserted=2, cycles_deasserted=1)
    tb.timeout(timeout)

    tb.drive("fmt_sel", fmt_sel_value)
    tb.drive("k_in", K)
    tb.drive("topk_drain_addr", 0)

    # ── Cycle 0: fire chunk 0 ──
    tb.drive("chunk_vals", chunk_v_buses[0])
    tb.drive("chunk_idxs", chunk_i_buses[0])
    tb.drive("valid_in", 1)
    tb.next()
    tb.drive("valid_in", 0)
    tb.drive("chunk_vals", 0)
    tb.drive("chunk_idxs", 0)
    cur_cy = 1

    # ── Loop fires chunks 1..n_chunks-1 every cy_per_chunk cycles ──
    for i in range(1, n_chunks):
        # advance until we are at the next fire window (cy_per_chunk · i)
        target_fire = cy_per_chunk * i
        while cur_cy < target_fire:
            tb.next()
            cur_cy += 1
        tb.drive("chunk_vals", chunk_v_buses[i])
        tb.drive("chunk_idxs", chunk_i_buses[i])
        tb.drive("valid_in", 1)
        tb.next()
        cur_cy += 1
        tb.drive("valid_in", 0)
        tb.drive("chunk_vals", 0)
        tb.drive("chunk_idxs", 0)

    # ── Drain to sample_row0 ──
    while cur_cy < sample_row0:
        tb.next()
        cur_cy += 1

    # ── Sample row 0 (drain_addr=0 has been held since cy 0) ──
    tb.expect(
        "running_valid", 1,
        msg=f"[{fmt_name}] running_valid must be sticky after first fire",
    )
    tb.expect(
        "ready_out", 1,
        msg=f"[{fmt_name}] ready must be 1 once chunks settle",
    )
    tb.expect(
        "topk_vals", row0_vals_bus,
        msg=f"[{fmt_name}] row 0 vals mismatch (top-{P} of K={K})",
    )
    tb.expect(
        "topk_idxs", row0_idxs_bus,
        msg=f"[{fmt_name}] row 0 idxs mismatch",
    )

    # ── Optionally sample row 1 ──
    if row1_vals_bus is not None:
        # Switch drain_addr to 1; sram_rdata/raddr_d need 2 cycles to settle.
        tb.next()
        cur_cy += 1
        tb.drive("topk_drain_addr", 1)
        tb.next()
        cur_cy += 1
        tb.next()
        cur_cy += 1
        tb.expect(
            "topk_vals", row1_vals_bus,
            msg=f"[{fmt_name}] row 1 vals mismatch (next-{P} of K={K})",
        )
        tb.expect(
            "topk_idxs", row1_idxs_bus,
            msg=f"[{fmt_name}] row 1 idxs mismatch",
        )

    tb.finish(at=finish_cy)
