from __future__ import annotations

from pycircuit import CycleAwareCircuit, CycleAwareDomain


def build(m: CycleAwareCircuit, domain: CycleAwareDomain) -> None:
    del domain
    value = m.input("a", width=8)
    tap = m.alias(value, name="tap")
    m.output("y", tap)


build.__pycircuit_name__ = "supernode_named_probe"
