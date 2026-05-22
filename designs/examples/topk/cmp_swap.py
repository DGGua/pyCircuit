"""(value, index) Compare-Swap cell with runtime ``fmt_sel``.

The cell takes two ``(val, idx)`` lanes A and B, a 1-bit ``dir``, and a
runtime 3-bit ``fmt_sel`` (selecting one of bf16/fp16/fp32/fp8_e4m3/fp4_e2m1).

  - ``dir = 1`` (DESC): output ``lo`` is the **larger** value (max-low).
  - ``dir = 0`` (ASC) : output ``lo`` is the **smaller** value (min-low).

Truth table (``lt = a < b`` from ``fp_lt``):

  ┌─────┬─────┬──────┬───────────────────────────┐
  │ dir │ lt  │ swap │ output (lo, hi)           │
  ├─────┼─────┼──────┼───────────────────────────┤
  │  1  │  1  │  1   │ (b, a)  — DESC, a < b     │
  │  1  │  0  │  0   │ (a, b)  — DESC, a ≥ b     │
  │  0  │  1  │  0   │ (a, b)  — ASC,  a < b     │
  │  0  │  0  │  1   │ (b, a)  — ASC,  a ≥ b     │
  └─────┴─────┴──────┴───────────────────────────┘

Hence ``swap = ~(dir ^ lt) = (dir == lt)``. Two independent muxes (val + idx)
keep indices following their values.

Value width is fixed at ``VAL_W`` (32). bf16/fp16 use the low 16 bits, fp8 the
low 8 bits, fp4 the low 4 bits; ``fp_lt`` honors ``fmt_sel`` to interpret them.
"""
from __future__ import annotations

from typing import Tuple

from pycircuit.hw import Wire

from fp_compare import fp_lt
from topk_config import VAL_W, FMT_SEL_W, FMT_FP32


def cmp_swap(
    val_a: Wire,
    idx_a: Wire,
    val_b: Wire,
    idx_b: Wire,
    dir_bit: Wire,
    fmt_sel: Wire,
) -> Tuple[Wire, Wire, Wire, Wire, int]:
    """Build a single (val, idx) cmp-swap cell with dynamic direction.

    Returns ``(val_lo, idx_lo, val_hi, idx_hi, depth)``.
    """
    if val_a.width != VAL_W or val_b.width != VAL_W:
        raise ValueError(
            f"cmp_swap: value width must be {VAL_W} (a={val_a.width}, b={val_b.width})"
        )
    if idx_a.width != idx_b.width:
        raise ValueError(
            f"cmp_swap: index width mismatch (a={idx_a.width}, b={idx_b.width})"
        )

    lt, d_lt = fp_lt(val_a, val_b, fmt_sel)
    swap = ~(dir_bit ^ lt)
    val_lo = swap.select(val_b, val_a)
    val_hi = swap.select(val_a, val_b)
    idx_lo = swap.select(idx_b, idx_a)
    idx_hi = swap.select(idx_a, idx_b)
    return val_lo, idx_lo, val_hi, idx_hi, d_lt + 2 + 2


def cmp_swap_const_dir(
    val_a: Wire,
    idx_a: Wire,
    val_b: Wire,
    idx_b: Wire,
    direction: int,
    fmt_sel: Wire,
) -> Tuple[Wire, Wire, Wire, Wire, int]:
    """Compile-time-direction variant: ``direction`` is a Python int (0 or 1).

    Saves one XOR layer when the schedule fixes the direction at synth time.
    """
    if direction not in (0, 1):
        raise ValueError(f"direction must be 0 or 1, got {direction}")
    if val_a.width != VAL_W or val_b.width != VAL_W:
        raise ValueError(
            f"cmp_swap_const_dir: value width must be {VAL_W} (a={val_a.width}, b={val_b.width})"
        )
    if idx_a.width != idx_b.width:
        raise ValueError(
            f"cmp_swap_const_dir: index width mismatch (a={idx_a.width}, b={idx_b.width})"
        )

    lt, d_lt = fp_lt(val_a, val_b, fmt_sel)
    if direction == 1:
        swap = lt
    else:
        swap = ~lt
    val_lo = swap.select(val_b, val_a)
    val_hi = swap.select(val_a, val_b)
    idx_lo = swap.select(idx_b, idx_a)
    idx_hi = swap.select(idx_a, idx_b)
    return val_lo, idx_lo, val_hi, idx_hi, d_lt + 1 + 2


# ═════════════════════════════════════════════════════════════════
# Build entry: instantiate a single cmp_swap cell and emit MLIR
# ═════════════════════════════════════════════════════════════════

def build(m, domain, *, idx_w: int = 12) -> None:
    va = m.input("va", width=VAL_W)
    vb = m.input("vb", width=VAL_W)
    ia = m.input("ia", width=idx_w)
    ib = m.input("ib", width=idx_w)
    dirw = m.input("dir", width=1)
    fmt_sel = m.input("fmt_sel", width=FMT_SEL_W)

    v_lo, i_lo, v_hi, i_hi, depth = cmp_swap(va, ia, vb, ib, dirw, fmt_sel)

    m.output("v_lo", v_lo)
    m.output("v_hi", v_hi)
    m.output("i_lo", i_lo)
    m.output("i_hi", i_hi)

    print(f"  cmp_swap: VAL_W={VAL_W}, idx_w={idx_w}, comb-depth={depth}")


if __name__ == "__main__":
    from pycircuit import compile_cycle_aware

    print("Building cmp_swap MLIR (unified runtime fmt_sel) ...")
    circuit = compile_cycle_aware(build, name="cmp_swap", idx_w=12)
    mlir = circuit.emit_mlir()
    print(f"  unified: MLIR {len(mlir)} chars")
    print("All cmp_swap builds passed.")
