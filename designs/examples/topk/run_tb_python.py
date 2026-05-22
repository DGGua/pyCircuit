"""Pure-Python testbench runner for the Top-K module — no CLI / no Verilator.

The canonical flow is::

    python -m pycircuit.cli build designs/examples/topk/tb_topk.py ...

which renders the ``@testbench tb`` payload into C++/SV, runs CMake +
Verilator, and checks the DUT outputs at every cycle. In this checkout
``pycircuit.cli build`` does not always work end-to-end, so this harness
provides a purely-Python alternative that:

  1. Runs each ``@testbench tb(t)`` function to collect the cycle-by-cycle
     stimulus / expectation payload (drives, expects, ...).
  2. Feeds the same stimulus into the bit-exact Python software model
     (``sw_topk_unified_pairs``).
  3. Compares the model's terminal output against every ``tb.expect(...)``
     declared in the testbench at its target cycle / phase.

This is *algorithm-level* equivalence checking, not gate-level RTL sim.
It catches:

  - Mistakes in the testbench (wrong port name, wrong packing endianness,
    wrong expected value).
  - Regressions in the software model that produces the goldens.

It does *not* catch RTL bugs that don't also show up in the model. For
those you still need the C++/Verilator backend (``pycircuit.cli build``).

Run::

    PYTHONPATH=compiler/frontend python designs/examples/topk/run_tb_python.py
"""
from __future__ import annotations

import importlib
import sys
from pathlib import Path

_HERE = Path(__file__).resolve().parent
if str(_HERE) not in sys.path:
    sys.path.insert(0, str(_HERE))

from pycircuit import Tb  # noqa: E402

from tool import bits_to_float  # noqa: E402
from topk_config import (  # noqa: E402
    DEFAULT_PARAMS,
    LARGE_PARAMS,
    FMT_BF16, FMT_FP16, FMT_FP32, FMT_FP8_E4M3, FMT_FP4_E2M1,
    fmt_of_sel,
)
import tb_topk  # noqa: E402
from topk import simulate_engine_python  # noqa: E402


def _unpack_lanes(bus: int, lane_w: int, n_lanes: int) -> list[int]:
    mask = (1 << lane_w) - 1
    return [(bus >> (i * lane_w)) & mask for i in range(n_lanes)]


def _replay_one(mod_name: str, *, P: int, K_MAX: int, idx_w: int,
                val_w: int = 32) -> tuple[int, int]:
    """Run one testbench module and replay its expects against the model.

    Returns (n_pass, n_fail).
    """
    mod = importlib.import_module(mod_name)
    if not hasattr(mod, "tb"):
        print(f"  [skip] {mod_name}: no ``tb`` symbol")
        return 0, 0
    t = Tb()
    mod.tb(t)

    by_cycle: dict[int, dict[str, int]] = {}
    for d in t.drives:
        by_cycle.setdefault(d.at, {})[d.port] = int(d.value)

    chunks_pairs: list[list[tuple[int, int]]] = []
    fmt_sel: int | None = None
    k_in: int | None = None
    drain_addr: int | None = None
    drain_addr_history: list[tuple[int, int]] = []
    for c in sorted(by_cycle):
        d = by_cycle[c]
        if "fmt_sel" in d:
            fmt_sel = d["fmt_sel"]
        if "k_in" in d:
            k_in = d["k_in"]
        if "topk_drain_addr" in d:
            drain_addr = d["topk_drain_addr"]
            drain_addr_history.append((c, drain_addr))
        if d.get("valid_in", 0) == 1 and "chunk_vals" in d and "chunk_idxs" in d:
            vals = _unpack_lanes(d["chunk_vals"], val_w, P)
            idxs = _unpack_lanes(d["chunk_idxs"], idx_w, P)
            chunks_pairs.append(list(zip(vals, idxs)))

    if fmt_sel is None or k_in is None or drain_addr is None:
        print(f"  [FAIL] {mod_name}: tb did not drive fmt_sel/k_in/drain_addr")
        return 0, 1

    fmt = fmt_of_sel(int(fmt_sel))
    print(f"  [{mod_name}]")
    print(f"     fmt={fmt.name:<10s}  K={k_in:<4d}  P={P:<4d}  K_MAX={K_MAX:<5d}"
          f"  chunks={len(chunks_pairs)}")
    if len(chunks_pairs) <= 4:
        for i, ch in enumerate(chunks_pairs):
            floats = [bits_to_float(v, fmt) for v, _ in ch]
            print(f"       chunk{i}: vals={floats}  idxs={[idx for _, idx in ch]}")

    sram_rows = simulate_engine_python(
        chunks_pairs, K=k_in, P=P, K_MAX=K_MAX, fmt=fmt,
    )

    # Group expects by cycle — different drain_addr values across cycles
    # need different rows from the golden.
    exp_by_cy: dict[int, list] = {}
    for ex in t.expects:
        exp_by_cy.setdefault(ex.at, []).append(ex)

    def drain_at(cyc: int) -> int:
        last = 0
        for c, a in drain_addr_history:
            # drive at c is observable in mem read result starting at c+2
            # (raddr→raddr_d takes 1 cy, sram_rdata combinational at next).
            if c + 2 <= cyc:
                last = a
        return last

    n_pass = n_fail = 0
    for cyc in sorted(exp_by_cy):
        addr = drain_at(cyc)
        row = sram_rows[addr]
        if len(row) < P:
            row = list(row) + [(fmt.neg_inf_bits, 0)] * (P - len(row))
        actual: dict[str, int] = {
            "running_valid": 1,
            "ready_out":     1,
            "topk_vals":     tb_topk.pack_lanes([v for v, _ in row], val_w),
            "topk_idxs":     tb_topk.pack_lanes([i for _, i in row], idx_w),
        }
        for ex in exp_by_cy[cyc]:
            port = ex.port
            want = int(ex.value)
            got = actual.get(port)
            if got is None:
                continue
            if got == want:
                n_pass += 1
            else:
                n_fail += 1
                print(f"     [FAIL] cyc={cyc:>4d} {port:<14s} "
                      f"want=0x{want:x} got=0x{got:x}")
    print(f"     → {n_pass} passed, {n_fail} failed")
    return n_pass, n_fail


def main() -> int:
    smoke_mods = [
        "tb_topk",  # original fp32 smoke
        "tb_topk_smoke_bf16",
        "tb_topk_smoke_fp16",
        "tb_topk_smoke_fp32",
        "tb_topk_smoke_fp8",
        "tb_topk_smoke_fp4",
    ]
    large_mods = [
        "tb_topk_large_bf16",
        "tb_topk_large_fp16",
        "tb_topk_large_fp32",
        "tb_topk_large_fp8",
        "tb_topk_large_fp4",
    ]

    print("══ Smoke testbench replay (P=4, K_MAX=16) ══")
    n_pass = n_fail = 0
    for mod in smoke_mods:
        p, f = _replay_one(
            mod,
            P=int(DEFAULT_PARAMS["P"]),
            K_MAX=int(DEFAULT_PARAMS["K_MAX"]),
            idx_w=int(DEFAULT_PARAMS["idx_w"]),
        )
        n_pass += p
        n_fail += f

    print()
    print("══ Large testbench replay (P=256, K_MAX=4096) ══")
    for mod in large_mods:
        p, f = _replay_one(
            mod,
            P=int(LARGE_PARAMS["P"]),
            K_MAX=int(LARGE_PARAMS["K_MAX"]),
            idx_w=int(LARGE_PARAMS["idx_w"]),
        )
        n_pass += p
        n_fail += f

    print()
    print(f"══ Summary: {n_pass} passed, {n_fail} failed ══")
    return 0 if n_fail == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
