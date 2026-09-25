"""Branches, jal, and jalr.

Taken ``beq`` skips an ``addi`` that would set x3 to 1. ``jal`` lands on an
``addi`` that sets x3 to 9. ``jalr`` returns over a second wrong ``addi``.
The mailbox store then publishes x3.
"""

from __future__ import annotations

import sys
from pathlib import Path

_TB = Path(__file__).resolve().parent
_ROOT = _TB.parent
for _p in (str(_TB), str(_ROOT)):
    if _p not in sys.path:
        sys.path.insert(0, _p)

from pycircuit import Tb, testbench

from common import run_until_halt
from lib.encode import addi, beq, bne, jal, jalr, sw
from scalar.core import elaborate


def _program() -> tuple[int, ...]:
    """Taken beq, not-taken bne, jal over a bad addi, jalr to the mailbox store.

    Words:
    0 addi x1, x0, 1
    1 addi x2, x0, 1
    2 beq x1, x2, +8      land on word 4
    3 addi x3, x0, 1      skipped
    4 bne x1, x2, +8      not taken
    5 jal x4, +12         link x4 = 24, land on word 8
    6 addi x3, x0, 2      skipped
    7 sw x3, 0(x0)
    8 addi x3, x0, 9
    9 jalr x0, x4, 4      24 + 4 = 28, word 7
    """
    return (
        addi(1, 0, 1),
        addi(2, 0, 1),
        beq(1, 2, 8),
        addi(3, 0, 1),
        bne(1, 2, 8),
        jal(4, 12),
        addi(3, 0, 2),
        sw(3, 0, 0),
        addi(3, 0, 9),
        jalr(0, 4, 4),
    )


def build(m, domain) -> None:
    elaborate(m, domain, _program())


build.__pycircuit_name__ = "coralnpu_branch"


@testbench
def tb(t: Tb) -> None:
    run_until_halt(t, instructions=8, tohost=9)
