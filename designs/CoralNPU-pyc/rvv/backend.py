"""Vector command port for the Coral NPU slice.

The scalar core owns the vector register file. This module only publishes the
command wires the tests already check. ``rvv_idle`` is low while a unit-stride
vector load or store is stepping through DTCM.
"""

from __future__ import annotations

from pycircuit import CycleAwareCircuit, u, wire_of


def tie_rvv(m: CycleAwareCircuit, idle) -> None:
    """Publish the vector command port. ``idle`` is 1 when no vector memory beat is in flight."""
    m.output("rvv_cmd_valid", u(1, 0))
    m.output("rvv_cmd_ready", u(1, 1))
    m.output("rvv_idle", wire_of(idle))
