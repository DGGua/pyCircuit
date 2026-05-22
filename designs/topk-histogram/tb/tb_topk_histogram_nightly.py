"""Top-K Histogram nightly testbench (K=900, EQ-saturating).

Nightly preset (from topk_histogram_config.TB_PRESETS["nightly"]):
    K=900, seed=42

For deterministic timing under the v1 pyCircuit JIT we use an all-same
stimulus (value=3.5). With all-same input and K=900:
  - gt_count = 0 (no element is strictly greater than the only value)
  - FILTER: 8 full cycles single pass (cy 7 writes the last 4 of 900),
    exits on wptr_next == K AND at_beat_last simultaneously
  - last beat's out_valid_mask = low 4 bits → exercises the partial-mask path

Random-seed coverage stays in Layer A's `_selftest` (which uses seed=42
random data and compares against argpartition); the nightly RTL tb keeps
timing deterministic so it can serve as a Layer B regression gate.

Layer B (canonical, CLI → Verilator):

    PYTHONPATH=compiler/frontend python -m pycircuit.cli build \
        designs/topk-histogram/tb/tb_topk_histogram_nightly.py \
        --out-dir .pycircuit_out/topk_histogram_nightly \
        --target both --run-verilator

Layer C (Python replay): triggered by `run_tb_python.py` automatically.
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
    p = TB_PRESETS["nightly"]
    K = int(p["K"])
    stim = gen_alleq_stimulus(K=K)

    tbh.clock("clk")
    tbh.reset("rst", cycles_asserted=2, cycles_deasserted=1)
    tbh.timeout(int(p["timeout"]))

    drive_request_and_load(tbh, stim)
    cur = 8
    cur = wait_until(tbh, stim.wait_out_cy, cur)
    expect_output_burst(tbh, stim)

    tbh.finish(at=int(p["finish"]))


if __name__ == "__main__":
    print(compile_cycle_aware(
        build, name="tb_topk_histogram_nightly_top", eager=True, **_build_params(),
    ).emit_mlir()[:2000])
