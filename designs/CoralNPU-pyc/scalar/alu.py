"""Single-cycle RV32I ALU used by the scalar execute stage.

Coral's scalar ALU is one cycle and there are four copies in the full core.
S1 instantiates one copy. Shift, compare, and bitwise ops match the RV32I
register and immediate forms that share ``funct3``.
"""

from __future__ import annotations

from pycircuit import CycleAwareSignal, mux, u


def alu_result(
    a: CycleAwareSignal,
    b: CycleAwareSignal,
    funct3: CycleAwareSignal,
    subtract: CycleAwareSignal,
    arith_shift: CycleAwareSignal,
) -> CycleAwareSignal:
    """Return the 32-bit ALU result for one RV32I arithmetic op.

    ``subtract`` selects ``sub`` over ``add``. ``arith_shift`` selects ``sra``
    over ``srl``. Both are zero for immediate ops other than ``srai``.
    """
    shamt = b[0:5]
    added = mux(subtract, a - b, a + b)
    shifted_l = a << shamt
    shifted_r = mux(arith_shift, a.ashr(amount=shamt), a.lshr(amount=shamt))
    slt = mux(a.as_signed() < b.as_signed(), u(32, 1), u(32, 0))
    sltu = mux(a.as_unsigned() < b.as_unsigned(), u(32, 1), u(32, 0))
    return mux(
        funct3 == u(3, 0),
        added,
        mux(
            funct3 == u(3, 1),
            shifted_l,
            mux(
                funct3 == u(3, 2),
                slt,
                mux(
                    funct3 == u(3, 3),
                    sltu,
                    mux(
                        funct3 == u(3, 4),
                        a ^ b,
                        mux(
                            funct3 == u(3, 5),
                            shifted_r,
                            mux(funct3 == u(3, 6), a | b, a & b),
                        ),
                    ),
                ),
            ),
        ),
    )
