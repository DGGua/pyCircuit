"""Integer vector ALU for VLEN=128, SEW=32, LMUL=1.

Four 32-bit lanes. ``vadd.vv``, ``vand.vv``, ``vor.vv``, and ``vxor.vv``.
The funct6 values are the RVV OPIVV encodings.
"""

from __future__ import annotations

from pycircuit import mux, u

from scalar.core_util import cat_imm


def vv_op(a, b, funct6):
    """One 128-bit result. ``a`` and ``b`` are the vs1 and vs2 registers."""
    lanes = []
    for i in range(4):
        lo = 32 * i
        ae = a[lo : lo + 32]
        be = b[lo : lo + 32]
        lanes.append(
            mux(
                funct6 == u(6, 0),
                ae + be,
                mux(funct6 == u(6, 9), ae & be, mux(funct6 == u(6, 10), ae | be, ae ^ be)),
            )
        )
    return cat_imm(lanes[3], lanes[2], lanes[1], lanes[0])


def insert_lane(vec, word, lane):
    """Replace one 32-bit lane. ``lane`` is the beat index, 0 through 3."""
    parts = []
    for i in range(4):
        parts.append(mux(lane == u(3, i), word, vec[32 * i : 32 * i + 32]))
    return cat_imm(parts[3], parts[2], parts[1], parts[0])


def extract_lane(vec, lane):
    """Read one 32-bit lane out of a 128-bit register."""
    return mux(
        lane == u(3, 0),
        vec[0:32],
        mux(lane == u(3, 1), vec[32:64], mux(lane == u(3, 2), vec[64:96], vec[96:128])),
    )
