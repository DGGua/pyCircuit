"""Top-K Histogram smoke testbench (Layer B + Layer C entry point).

Layer B (canonical, CLI → Verilator):

    PYTHONPATH=compiler/frontend python -m pycircuit.cli build \
        designs/topk-histogram/tb/tb_topk_histogram.py \
        --out-dir .pycircuit_out/topk_histogram \
        --target both --run-verilator

Layer C (Python replay against the software model):

    PYTHONPATH=compiler/frontend python designs/topk-histogram/tb/run_tb_python.py

Smoke preset (from topk_histogram_config.TB_PRESETS["smoke"]):
    K=4, seed=0, deterministic all-same stimulus (value=3.5)

With all-same input, the single-pass FILTER takes ceil(K/128) cycles:
    K=4 → FILTER = 1 cy, out_req at cy 70.
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

from pycircuit import CycleAwareTb, Tb, compile_cycle_aware, testbench  # noqa: E402

from topk_histogram import build, _build_params  # noqa: E402
from topk_histogram_config import TB_PRESETS  # noqa: E402

from tb_topk_histogram_smoke_lib import (  # noqa: E402
    drive_request_and_load,
    expect_output_burst,
    gen_alleq_stimulus,
    wait_until,
)


@testbench
def tb(t: Tb) -> None:
    tbh = CycleAwareTb(t)
    p = TB_PRESETS["smoke"]
    K = int(p["K"])
    stim = gen_alleq_stimulus(K=K)

    tbh.clock("clk")
    tbh.reset("rst", cycles_asserted=2, cycles_deasserted=1)
    tbh.timeout(int(p["timeout"]))

    # cycle 0 — REQ
    drive_request_and_load(tbh, stim)

    # Now positioned at cycle 8 (last LOAD beat). Advance to wait_out_cy.
    cur = 8
    cur = wait_until(tbh, stim.wait_out_cy, cur)

    # cycle == stim.wait_out_cy: out_req should be asserted.
    expect_output_burst(tbh, stim)

    tbh.finish(at=int(p["finish"]))


if __name__ == "__main__":
    print(compile_cycle_aware(
        build, name="tb_topk_histogram_top", eager=True, **_build_params(),
    ).emit_mlir()[:2000])
