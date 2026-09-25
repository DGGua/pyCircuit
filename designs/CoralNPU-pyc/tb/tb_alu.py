"""RV32I arithmetic, logic, and shift sequence.

The mailbox receives ``x3``, which is the AND of two intermediate results.
Each intermediate checks a different ALU class: add/sub, shifts, compares,
and bitwise ops. A wrong op fails the final ``tohost`` value.
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
from lib.encode import addi, add, and_, or_, sll, slt, sltu, sra, srl, sub, sw, xor
from scalar.core import elaborate


def build(m, domain) -> None:
    elaborate(m, domain, _program())


def _program() -> tuple[int, ...]:
    words = [
        addi(1, 0, 5),
        addi(2, 0, 3),
        add(3, 1, 2),
        sub(4, 1, 2),
        addi(6, 0, 1),
        sll(5, 3, 6),
        srl(7, 5, 6),
        addi(9, 0, -8),
        sra(8, 9, 6),
        addi(11, 0, 1),
        addi(12, 0, 2),
        slt(10, 11, 12),
        sltu(13, 11, 12),
        addi(14, 0, 0xFF),
        addi(15, 0, 0x0F),
        xor(14, 14, 15),
        or_(15, 14, 15),
        and_(3, 15, 14),
        sw(3, 0, 0),
    ]
    return tuple(words)


build.__pycircuit_name__ = "coralnpu_alu"


@testbench
def tb(t: Tb) -> None:
    # 0xf0 | 0x0f = 0xff; 0xff & 0xf0 = 0xf0.
    run_until_halt(t, instructions=19, tohost=0xF0)
