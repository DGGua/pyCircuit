from __future__ import annotations

from pycircuit import Circuit, module, u


@module
def child(m: Circuit, a):
    """A preserved hierarchy boundary with combinational logic on both sides."""

    return (a ^ u(8, 0x5A)) + u(8, 1)


@module
def build(m: Circuit) -> None:
    a = m.input("a", width=8)
    b = m.input("b", width=8)
    before = a + u(8, 3)
    through_child = m.instance(child, name="u_child", a=before).read()
    after = through_child ^ b
    m.output("pre_out", before)
    m.output("post_out", after)


build.__pycircuit_name__ = "supernode_instance_boundary"
