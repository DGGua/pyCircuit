from __future__ import annotations

from pycircuit import CycleAwareCircuit, CycleAwareDomain, compile_cycle_aware, u


def build(m: CycleAwareCircuit, domain: CycleAwareDomain) -> None:
    """Pure-comb stress shape for the SuperNode optional-update gates.

    The first ``and`` is deliberately a producer with two fanouts.  With
    ``mask == 0``, changing ``a`` exercises the important case where a producer
    runs but its semantic output is unchanged, so neither fanout should run.
    The two fanouts reconverge at ``result``.  Wide and vector-shaped paths make
    sure partition live-ins/live-outs and equality guards are not scalar-only.
    ``constant_out`` supplies a zero-input partition candidate.
    """

    reset_seen = domain.create_reset()
    a = m.input("a", width=8)
    mask = m.input("mask", width=8)
    b = m.input("b", width=8)
    c = m.input("c", width=8)
    assert_ok = m.input("assert_ok", width=1)

    producer = a & mask
    # This side-effecting operation deliberately interrupts the two runs that
    # legacy FuseComb sees.  The static partition path must ignore physical
    # run boundaries and recover the producer -> consumers relation from the
    # unified function-level SSA CombDepGraph.
    m.assert_(assert_ok, msg="unified-comb-boundary")
    left = (producer + b) ^ c
    right = (producer + c) ^ b
    result = left ^ right

    wide = m.input("wide", width=130)
    wide_mask = m.input("wide_mask", width=130)
    wide_bias = m.input("wide_bias", width=130)
    wide_producer = wide & wide_mask
    wide_out = wide_producer ^ wide_bias

    vec_a = m.vec([m.input(f"vec_a{i}", width=8) for i in range(4)])
    vec_mask = m.vec([m.input(f"vec_mask{i}", width=8) for i in range(4)])
    vec_bias = m.vec([m.input(f"vec_bias{i}", width=8) for i in range(4)])
    vec_producer = vec_a & vec_mask
    vec_out = vec_producer + vec_bias

    m.output("producer", producer)
    m.output("result", result)
    m.output("wide_out", wide_out)
    for i in range(4):
        m.output(f"vec_out{i}", vec_out[i])
    m.output("constant_out", u(8, 0x5A))
    m.output("reset_seen", reset_seen)


build.__pycircuit_name__ = "supernode_optional_update"


if __name__ == "__main__":
    print(
        compile_cycle_aware(
            build,
            name="supernode_optional_update",
            eager=True,
            hierarchical=True,
        ).emit_mlir()
    )
