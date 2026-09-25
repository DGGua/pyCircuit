"""Short nop sequence ending in a mailbox store.

Coral's cocotb ``nop_test.cc`` loops 100 times over 512 nops and writes a CSR.
CSR writes and that loop length are outside the S1 integer slice, so this
program keeps the architectural intent: nops retire, then a store reports
completion through ``tohost``.
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
from lib.encode import addi, sw
from scalar.core import elaborate

PROGRAM = (
    addi(0, 0, 0),
    addi(0, 0, 0),
    addi(0, 0, 0),
    addi(10, 0, 1),
    sw(10, 0, 0),
)


def build(m, domain) -> None:
    elaborate(m, domain, PROGRAM)


build.__pycircuit_name__ = "coralnpu_nop"


@testbench
def tb(t: Tb) -> None:
    run_until_halt(t, instructions=5, tohost=1)
