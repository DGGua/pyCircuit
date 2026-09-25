"""Placeholder for Coral's matrix-tile file.

The cloned Coral tree documents 16 matrix tiles and an outer-product GEMM
engine, but it does not contain a runnable PE array. S1 keeps one observable
tile word at zero and does not execute ``mset*`` or GEMM.
"""

from __future__ import annotations

from pycircuit import CycleAwareCircuit, u


def tie_matrix(m: CycleAwareCircuit) -> None:
    """Expose the first matrix tile word. It is never written in S1."""
    m.output("matrix_tile0", u(32, 0))
    m.output("matrix_idle", u(1, 1))
