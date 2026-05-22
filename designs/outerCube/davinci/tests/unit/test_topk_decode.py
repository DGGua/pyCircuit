"""Unit tests for TTOPK decode/control extraction."""

from __future__ import annotations

import os
import sys

_root = os.path.join(os.path.dirname(__file__), "..", "..", "..", "..", "..")
sys.path.insert(0, os.path.join(_root, "compiler", "frontend"))
sys.path.insert(0, _root)

from pycircuit import CycleAwareTb, compile_cycle_aware
from pycircuit.tb import Tb

from designs.outerCube.davinci.common.parameters import VEC_OP_TOPK


def decode_topk_test(m, domain) -> None:
    from designs.outerCube.davinci.frontend.decode.decode import decoder

    decoder(m, domain, width=1, prefix="dec")


decode_topk_test.__pycircuit_name__ = "test_topk_decode"


def _topk_instr(*, rd: int = 3, src: int = 4, mask: int = 5, k: int = 5, has_mask: bool = True) -> int:
    funct7 = (k & 0b111) | ((1 if has_mask else 0) << 6)
    return (
        VEC_OP_TOPK
        | ((rd & 0x1F) << 7)
        | ((src & 0x1F) << 15)
        | ((mask & 0x1F) << 20)
        | (funct7 << 25)
    )


def test_topk_decode_compile():
    """Decoder exposes staged TTOPK control signals."""
    circuit = compile_cycle_aware(decode_topk_test, name="test_topk_decode", eager=True)
    mlir = circuit.emit_mlir()
    assert "dec_is_vec0" in mlir
    assert "dec_topk_has_mask0" in mlir
    assert "dec_topk_k0" in mlir
    assert "dec_has_tdst2_0" in mlir
    print(f"PASS: TTOPK decode compile OK ({len(mlir):,} chars MLIR)")


def test_topk_decode_tb():
    """Generate a decode testbench for masked TTOPK K=5."""
    compile_cycle_aware(decode_topk_test, name="test_topk_decode", eager=True)
    t = Tb()
    ct = CycleAwareTb(t)
    ct.clock("clk")
    ct.reset("rst", cycles_asserted=2, cycles_deasserted=1)
    ct.timeout(20)

    ct.drive("dec_valid0", 1)
    ct.drive("dec_instr0", _topk_instr(k=5, has_mask=True))
    ct.expect("dec_is_vec0", 1)
    ct.expect("dec_topk_has_mask0", 1)
    ct.expect("dec_topk_k0", 5)
    ct.expect("dec_has_trs2_0", 1)
    ct.expect("dec_has_tdst2_0", 1)
    ct.next()
    ct.finish()

    assert len(t.drives) == 2
    assert len(t.expects) == 5
    print("PASS: TTOPK decode testbench generated")


if __name__ == "__main__":
    test_topk_decode_compile()
    test_topk_decode_tb()
    print("\nAll TTOPK decode tests passed!")
