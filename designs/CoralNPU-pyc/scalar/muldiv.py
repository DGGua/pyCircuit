"""RV32M multiply and one divider step.

The multiplier result is combinational. The scalar core holds the PC for one
extra cycle so the product commits on the second cycle, matching Coral's
2-cycle MLU. The divider steps one bit per cycle, the same restoring step
Coral's DVU uses, and the core blocks until 32 steps finish.
"""

from __future__ import annotations

from pycircuit import CycleAwareSignal, mux, u

from scalar.core_util import cat_imm


def mul_result(a: CycleAwareSignal, b: CycleAwareSignal, funct3: CycleAwareSignal) -> CycleAwareSignal:
    """Low or high half of a 32×32 product. ``funct3`` is the RV32M field."""
    prod_ss = a.sext(64) * b.sext(64)
    prod_su = a.sext(64) * b.zext(64)
    prod_uu = a.zext(64) * b.zext(64)
    high = mux(
        funct3 == u(3, 1),
        prod_ss[32:64],
        mux(funct3 == u(3, 2), prod_su[32:64], prod_uu[32:64]),
    )
    return mux(funct3 == u(3, 0), prod_ss[0:32], high)


def div_step(quot: CycleAwareSignal, rem: CycleAwareSignal, denom: CycleAwareSignal):
    """One restoring division bit. Returns ``(quot_next, rem_next)``."""
    shifted = cat_imm(rem[0:31], quot[31:32])
    diff = shifted.zext(33) - denom.zext(33)
    borrow = diff[32:33]
    rem_next = mux(borrow, shifted, diff[0:32])
    bit = mux(borrow, u(1, 0), u(1, 1))
    quot_next = cat_imm(quot[0:31], bit)
    return quot_next, rem_next
