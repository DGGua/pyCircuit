"""Placeholder for Coral's decoupled RVV backend.

S1 does not dispatch vector instructions. The port stays idle so the top-level
boundary matches the scalar / vector / matrix split. Integer vector ALU, MAC,
and load/store arrive in a later stage.
"""

from __future__ import annotations

from pycircuit import CycleAwareCircuit, u


def tie_rvv(m: CycleAwareCircuit) -> None:
    """Publish a ready-and-idle vector command port with no queued command."""
    m.output("rvv_cmd_valid", u(1, 0))
    m.output("rvv_cmd_ready", u(1, 1))
    m.output("rvv_idle", u(1, 1))
