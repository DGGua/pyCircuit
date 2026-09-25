"""Multiply (2 cycles) and blocking divide/remainder.

``20 * 6 = 120``, ``120 / 7 = 17`` remainder ``1``. Signed ``(-8) / 3 = -2``
is added and then cancelled, so a wrong quotient misses ``tohost == 18``.
Each divide occupies 34 cycles: the issue cycle, 32 bit steps, and writeback.
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
from lib.encode import add, addi, div, divu, mul, remu, sw
from scalar.core import elaborate


def build(m, domain) -> None:
    elaborate(m, domain, _program())


def _program() -> tuple[int, ...]:
    return (
        addi(1, 0, 20),
        addi(2, 0, 6),
        mul(3, 1, 2),
        addi(4, 0, 7),
        divu(5, 3, 4),
        remu(6, 3, 4),
        add(7, 5, 6),
        addi(8, 0, -8),
        addi(9, 0, 3),
        div(10, 8, 9),
        add(7, 7, 10),
        addi(7, 7, 2),
        sw(7, 0, 0),
    )


build.__pycircuit_name__ = "coralnpu_muldiv"


@testbench
def tb(t: Tb) -> None:
    # 9 single-cycle ops, one mul (2), three div/rem (34 each).
    run_until_halt(t, instructions=13, tohost=18, cycles=9 + 2 + 34 * 3)
