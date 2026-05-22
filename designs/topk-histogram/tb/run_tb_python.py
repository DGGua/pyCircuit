"""Layer C: replay each smoke / nightly testbench against the Python model.

Why this exists:
  - Layer A (`topk_histogram_model._selftest`) checks the algorithm itself.
  - Layer B (`pycircuit.cli build ... --run-verilator`) checks the RTL.
  - Layer C (this file) checks that the *testbench* drives + expects are
    self-consistent: the drives reconstruct the input we *think* we're
    sending, and the expects match what the Python model says the DUT
    should produce for that input.

A pass here doesn't prove the RTL is correct, but a fail catches the most
common bug (wrong port name, wrong bit packing, stale golden value, etc.)
before you spend 10 minutes waiting for Verilator.

Run::

    PYTHONPATH=compiler/frontend python designs/topk-histogram/tb/run_tb_python.py
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

import importlib  # noqa: E402
from typing import List, Tuple  # noqa: E402

from pycircuit import Tb  # noqa: E402

from topk_histogram_config import DEFAULT_PARAMS  # noqa: E402
from topk_histogram_model import simulate_histogram_python  # noqa: E402
from tool import unpack_lanes  # noqa: E402


N = DEFAULT_PARAMS["N"]
LANE_NUM = DEFAULT_PARAMS["LANE_NUM"]
BURST_LEN = DEFAULT_PARAMS["BURST_LEN"]


def _collect_drives_expects(t: Tb) -> Tuple[dict, dict]:
    """Bucket the tb's drives/expects by cycle for easy lookup."""
    by_cycle_drives: dict = {}
    for d in t.drives:
        by_cycle_drives.setdefault(d.at, {})[d.port] = int(d.value)
    by_cycle_expects: dict = {}
    for e in t.expects:
        by_cycle_expects.setdefault(e.at, []).append((e.port, int(e.value)))
    return by_cycle_drives, by_cycle_expects


def _replay(mod_name: str) -> Tuple[int, int]:
    """Replay one testbench module. Returns (n_pass, n_fail)."""
    mod = importlib.import_module(mod_name)
    if not hasattr(mod, "tb"):
        print(f"  [skip] {mod_name}: no `tb` symbol")
        return 0, 0

    t = Tb()
    mod.tb(t)

    drives, expects = _collect_drives_expects(t)

    # Reconstruct LOAD inputs from drives at cycles 1..8
    K = None
    in_beats: List[List[int]] = []
    for cy in range(0, 1 + BURST_LEN):
        d = drives.get(cy, {})
        if cy == 0:
            K = d.get("cfg_topk", K)
        if cy >= 1:
            bus = d.get("in_data", 0)
            in_beats.append(unpack_lanes(bus, lane_w=32, n_lanes=LANE_NUM))
    if K is None:
        print(f"  [{mod_name}] FAIL: cfg_topk not driven at cycle 0")
        return 0, 1
    if len(in_beats) != BURST_LEN:
        print(
            f"  [{mod_name}] FAIL: expected {BURST_LEN} LOAD beats, got "
            f"{len(in_beats)}"
        )
        return 0, 1

    # Run the Python model on the reconstructed input
    golden = simulate_histogram_python(in_beats, K=K, N=N, LANE_NUM=LANE_NUM)

    # Find the drain window: the first cycle where out_req is expected to be 1.
    out_req_cycles = [
        cy for cy, exs in expects.items() if any(p == "out_req" and v == 1 for p, v in exs)
    ]
    if not out_req_cycles:
        print(f"  [{mod_name}] FAIL: testbench does not expect out_req=1 anywhere")
        return 0, 1
    wait_out_cy = min(out_req_cycles)
    drain_start_cy = wait_out_cy + 1

    n_pass = 0
    n_fail = 0
    for cy, exs in sorted(expects.items()):
        for port, val in exs:
            got = _model_value_for(
                port, cy,
                wait_out_cy=wait_out_cy,
                drain_start_cy=drain_start_cy,
                golden=golden,
            )
            if got is None:
                # No model mapping → ignore (e.g. internal probes)
                continue
            if got == val:
                n_pass += 1
            else:
                n_fail += 1
                print(
                    f"  [{mod_name}] FAIL cy={cy} port={port}: "
                    f"expect=0x{val:x} model=0x{got:x}"
                )

    return n_pass, n_fail


def _model_value_for(
    port: str, cy: int,
    *, wait_out_cy: int, drain_start_cy: int, golden,
) -> int | None:
    """Return what the Python model would put on ``port`` at cycle ``cy``."""
    if port == "out_req":
        return 1 if cy == wait_out_cy else 0
    beat = cy - drain_start_cy
    if not (0 <= beat < BURST_LEN):
        return None
    if port == "out_value":
        return golden.out_value_beats[beat]
    if port == "out_index_data":
        return golden.out_index_beats[beat]
    if port == "out_valid_mask":
        return golden.out_valid_mask_beats[beat]
    return None


def main(argv: list[str]) -> int:
    targets = ["tb_topk_histogram"]
    # nightly tb (only if it exists)
    try:
        importlib.import_module("tb_topk_histogram_nightly")
        targets.append("tb_topk_histogram_nightly")
    except ImportError:
        pass

    total_pass = 0
    total_fail = 0
    for tgt in targets:
        print(f"Replaying {tgt}...")
        np, nf = _replay(tgt)
        print(f"  {tgt}: pass={np} fail={nf}")
        total_pass += np
        total_fail += nf

    print(f"\nTotal: pass={total_pass} fail={total_fail}")
    return 0 if total_fail == 0 else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
