"""Shared testbench schedule for the single-issue Coral NPU slice.

The core commits one instruction per clock after reset. ``halt_cycle`` is the
cycle on which ``halted`` is first 1, counted the same way as the counter
example: cycle 0 is the first cycle the testbench observes after reset.
"""

from __future__ import annotations

import sys
from pathlib import Path

from pycircuit import CycleAwareTb, Tb

_ROOT = Path(__file__).resolve().parents[1]
if str(_ROOT) not in sys.path:
    sys.path.insert(0, str(_ROOT))


def run_until_halt(t: Tb, *, instructions: int, tohost: int) -> None:
    """Step once per instruction, then check the mailbox and the halt bit.

    Instruction 0 commits on the clock that produces cycle-0 outputs, matching
    ``designs/examples/counter``. The halt store is the last instruction, so
    ``halted`` is 1 on cycle ``instructions`` (the commit after the store).
    """
    tb = CycleAwareTb(t)
    tb.clock("clk")
    tb.reset("rst", cycles_asserted=2, cycles_deasserted=1)
    tb.timeout(64)
    for _ in range(instructions):
        tb.next()
    tb.expect("halted", 1)
    tb.expect("tohost", tohost)
    tb.expect("rvv_idle", 1)
    tb.expect("matrix_idle", 1)
    tb.expect("matrix_tile0", 0)
    tb.finish(at=instructions)
