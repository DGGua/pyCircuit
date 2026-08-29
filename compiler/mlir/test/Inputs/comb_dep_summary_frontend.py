from __future__ import annotations

from pycircuit import Circuit, Tb, module, testbench, u


@module
def child(m: Circuit, a):
    return (a ^ u(8, 0x5A)) + u(8, 1)


@module
def build(m: Circuit) -> None:
    a = m.input("a", width=8)
    b = m.input("b", width=8)
    child_y = m.instance(child, name="u_child", a=a).read()
    m.output("y", child_y ^ b)


@testbench
def tb(t: Tb) -> None:
    t.drive("a", 1, at=0)
    t.drive("b", 0x0F, at=0)
    t.expect("y", 0x53, at=0, phase="pre")
    t.finish(at=0)


build.__pycircuit_name__ = "comb_dep_summary_frontend"
