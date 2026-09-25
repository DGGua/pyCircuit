"""VLEN=128 integer vector add, logic, and unit-stride load/store.

Four e32 lanes. DTCM words 1, 2, 3, 4 are loaded into v1, doubled, masked
with a zero xor, and stored back. The mailbox is 2+4+6+8 = 20. This matches
the ``vle`` / ``vadd.vv`` shape in Coral's ``tests/cocotb/rvv/rvv_add.S``,
at SEW=32 instead of the e8 loop in that file.
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
from lib.encode import add, addi, lui, lw, sw, vand_vv, vadd_vv, vle32, vor_vv, vse32, vsetvli, vxor_vv
from scalar.core import elaborate


def build(m, domain) -> None:
    elaborate(m, domain, _program())


def _program() -> tuple[int, ...]:
    return (
        lui(1, 0x10),
        addi(2, 0, 1),
        sw(2, 1, 0),
        addi(2, 0, 2),
        sw(2, 1, 4),
        addi(2, 0, 3),
        sw(2, 1, 8),
        addi(2, 0, 4),
        sw(2, 1, 12),
        vsetvli(0, 0),
        vle32(1, 1),
        vadd_vv(2, 1, 1),
        vxor_vv(4, 1, 1),
        vand_vv(3, 2, 2),
        vor_vv(5, 3, 4),
        addi(6, 1, 16),
        vse32(5, 6),
        lw(3, 1, 16),
        lw(4, 1, 20),
        add(3, 3, 4),
        lw(4, 1, 24),
        add(3, 3, 4),
        lw(4, 1, 28),
        add(3, 3, 4),
        sw(3, 0, 0),
    )


build.__pycircuit_name__ = "coralnpu_rvv"


@testbench
def tb(t: Tb) -> None:
    run_until_halt(t, instructions=25, tohost=20, cycles=120)
