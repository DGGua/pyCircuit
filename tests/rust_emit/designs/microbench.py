"""Combinational-dense microbench for C++ vs Rust Hz comparison.

A chain of add/mux stages feeding a register each cycle, so the comparison is
about Wire arithmetic + eval/tick rather than testbench or memory.
"""

from __future__ import annotations

from pycircuit import (
    CycleAwareCircuit,
    CycleAwareDomain,
    cas,
    compile_cycle_aware,
    mux,
    wire_of,
)

STAGES = 32
WIDTH = 16


def build(m: CycleAwareCircuit, domain: CycleAwareDomain, stages: int = STAGES, width: int = WIDTH) -> None:
    sel = cas(domain, m.input("sel", width=1), cycle=0)
    addend = cas(domain, m.input("addend", width=width), cycle=0)
    acc = domain.signal(width=width, reset_value=0, name="acc")
    nxt = acc
    for _ in range(int(stages)):
        nxt = mux(sel, nxt + addend, nxt)
    domain.next()
    acc <<= nxt
    m.output("acc_out", wire_of(acc))


build.__pycircuit_name__ = "microbench"


if __name__ == "__main__":
    print(compile_cycle_aware(build, name="microbench", eager=True, stages=STAGES, width=WIDTH).emit_mlir())
