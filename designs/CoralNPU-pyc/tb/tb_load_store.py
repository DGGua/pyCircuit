"""Byte, halfword, and unaligned word accesses in the DTCM window.

DTCM starts at ``0x00010000``. The final mailbox value is
``0x7B + 0x11 + (-2) = 0x8A``. A wrong width, sign extension, or unaligned
store changes that sum.
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
from lib.encode import add, addi, lbu, lh, lui, sb, sh, sw
from scalar.core import elaborate


def build(m, domain) -> None:
    elaborate(m, domain, _program())


def _program() -> tuple[int, ...]:
    return (
        lui(1, 0x10),
        addi(2, 0, 0x7B),
        sb(2, 1, 1),
        lbu(4, 1, 1),
        addi(5, 0, -2),
        sh(5, 1, 4),
        lh(6, 1, 4),
        addi(8, 0, 0x11),
        sw(8, 1, 5),
        lbu(9, 1, 5),
        add(10, 4, 9),
        add(10, 10, 6),
        sw(10, 0, 0),
    )


build.__pycircuit_name__ = "coralnpu_load_store"


@testbench
def tb(t: Tb) -> None:
    run_until_halt(t, instructions=13, tohost=0x8A)
