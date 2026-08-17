from __future__ import annotations

from pycircuit import CycleAwareCircuit, CycleAwareDomain, mux, wire_of


def build(m: CycleAwareCircuit, domain: CycleAwareDomain) -> None:
    enable = m.input("enable", width=1)
    state = domain.signal(width=8, reset_value=0, name="state")
    current = wire_of(state)
    next_value = mux(enable, current + 1, current)
    m.output("current", current)
    domain.next()
    state <<= next_value


build.__pycircuit_name__ = "supernode_state_boundary"
