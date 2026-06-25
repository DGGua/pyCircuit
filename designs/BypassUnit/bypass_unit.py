from __future__ import annotations

from typing import cast

from pycircuit import (
    CycleAwareCircuit,
    CycleAwareDomain,
    Tb,
    Vec,
    compile_cycle_aware,
    function,
    mux,
    testbench,
)

PTYPE_C = 0
PTYPE_P = 1
PTYPE_T = 2
PTYPE_U = 3


@function
def _not1(m, x):
    return m.const(1, width=1) ^ x


@function
def _select_stage_batch(
    m,
    *,
    src_valid_v: Vec,
    src_ptag_v: Vec,
    src_ptype_v: Vec,
    lane_valid: Vec,
    lane_ptag: Vec,
    lane_ptype: Vec,
    lane_data: Vec,
    lane_nums: Vec,
    zero_lane,
    zero_data,
    lanes_n: int,
):
    """Batch bypass search: N sources × M lanes outer-product, one call."""
    _ = m
    n = int(lanes_n)
    # Outer product via broadcast.
    # src: Vec<N> → broadcast dim=1, size=M → Vec<N, M> (row-replicated)
    # lane: Vec<M> → broadcast dim=0, size=N → Vec<N, M> (col-replicated)
    sv_bc = src_valid_v.broadcast(dim=1, size=n)
    sp_bc = src_ptag_v.broadcast(dim=1, size=n)
    st_bc = src_ptype_v.broadcast(dim=1, size=n)

    lv_bc = lane_valid.broadcast(dim=0, size=n)
    lp_bc = lane_ptag.broadcast(dim=0, size=n)
    lt_bc = lane_ptype.broadcast(dim=0, size=n)

    match = sv_bc & lv_bc & (lp_bc == sp_bc) & (lt_bc == st_bc)
    # match: Vec<Vec<i1, M>, N> — N rows, each row is M match bits

    has = match.or_reduce(dim=1)  # Vec<i1, N>: per-source hit

    # Per-source priority_mux over the match row.
    sel_lane = Vec([match[i].priority_mux(lane_nums, zero=zero_lane) for i in range(n)])
    # Broadcast lane_data to match shape, then per-row priority_mux
    ld_bc = lane_data.broadcast(dim=0, size=n)
    sel_data = Vec([match[i].priority_mux(ld_bc[i], zero=zero_data) for i in range(n)])

    return has, sel_lane, sel_data


def build(
    m: CycleAwareCircuit,
    domain: CycleAwareDomain,
    *,
    lanes: int = 8,
    data_width: int = 64,
    ptag_count: int = 256,
    ptype_count: int = 4,
) -> None:
    lanes_n = int(lanes)
    data_w = int(data_width)
    ptag_n = int(ptag_count)
    ptype_n = int(ptype_count)

    if lanes_n <= 0:
        raise ValueError("bypass_unit lanes must be > 0")
    if data_w <= 0:
        raise ValueError("bypass_unit data_width must be > 0")
    if ptag_n <= 0:
        raise ValueError("bypass_unit ptag_count must be > 0")
    if ptype_n <= 0:
        raise ValueError("bypass_unit ptype_count must be > 0")
    if ptype_n <= PTYPE_U:
        raise ValueError("bypass_unit ptype_count must be >= 4 to represent C/P/T/U")

    ptag_w = max(1, (ptag_n - 1).bit_length())
    ptype_w = max(1, (ptype_n - 1).bit_length())
    lane_w = max(1, (lanes_n - 1).bit_length())

    # Pre-compute constants for the vectorised bypass search.
    # Use m.const: u() returns LiteralValue which Vec cannot ingest.
    lane_nums = Vec([m.const(j, width=lane_w) for j in range(int(lanes_n))])
    zero_hit = m.const(0, width=1)
    one_hit = m.const(1, width=1)
    zero_stage = m.const(0, width=2)
    stage_consts = {prio: m.const(prio, width=2) for prio in (1, 2, 3)}
    zero_lane = m.const(0, width=lane_w)
    zero_data = m.const(0, width=data_w)

    # Write-back stages as Vecs (one Vec per stage, size = lanes).
    w_valid: dict[str, Vec] = {}
    w_ptag: dict[str, Vec] = {}
    w_ptype: dict[str, Vec] = {}
    w_data: dict[str, Vec] = {}
    for stage in ("w1", "w2", "w3"):
        w_valid[stage] = cast(Vec, m.input(f"{stage}_valid", width=1, shape=lanes_n))
        w_ptag[stage] = cast(Vec, m.input(f"{stage}_ptag", width=ptag_w, shape=lanes_n))
        w_ptype[stage] = cast(Vec, m.input(f"{stage}_ptype", width=ptype_w, shape=lanes_n))
        w_data[stage] = cast(Vec, m.input(f"{stage}_data", width=data_w, shape=lanes_n))

    for src in ("srcL", "srcR"):
        # Collect all N sources of this type into Vecs.
        src_valid_v = cast(Vec, m.input(f"i2_{src}_valid", width=1, shape=lanes_n))
        src_ptag_v  = cast(Vec, m.input(f"i2_{src}_ptag", width=ptag_w, shape=lanes_n))
        src_ptype_v = cast(Vec, m.input(f"i2_{src}_ptype", width=ptype_w, shape=lanes_n))
        src_rfdata_v = cast(Vec, m.input(f"i2_{src}_rf_data", width=data_w, shape=lanes_n))

        sel_data  = src_rfdata_v
        sel_hit   = Vec([zero_hit for _ in range(lanes_n)])
        sel_stage = Vec([zero_stage for _ in range(lanes_n)])
        sel_lane  = Vec([zero_lane for _ in range(lanes_n)])

        # Priority: w3 > w2 > w1.
        for stage, prio in [("w3", 3), ("w2", 2), ("w1", 1)]:
            has, lane_sel, data_sel = _select_stage_batch(
                m,
                src_valid_v=src_valid_v, src_ptag_v=src_ptag_v, src_ptype_v=src_ptype_v,
                lane_valid=w_valid[stage], lane_ptag=w_ptag[stage],
                lane_ptype=w_ptype[stage], lane_data=w_data[stage],
                lane_nums=lane_nums, zero_lane=zero_lane, zero_data=zero_data,
                lanes_n=lanes_n,
            )

            sel_data  = mux(has, data_sel, sel_data)
            sel_hit   = mux(has, one_hit, sel_hit)
            sel_stage = mux(has, stage_consts[prio], sel_stage)
            sel_lane  = mux(has, lane_sel, sel_lane)

        m.output(f"i2_{src}_data", cast(Vec, sel_data))
        m.output(f"i2_{src}_hit", cast(Vec, sel_hit))
        m.output(f"i2_{src}_sel_stage", cast(Vec, sel_stage))
        m.output(f"i2_{src}_sel_lane", cast(Vec, sel_lane))


build.__pycircuit_name__ = "bypass_unit"


@testbench
def tb(t: Tb) -> None:
    t.clock("clk")
    t.reset("rst", cycles_asserted=1, cycles_deasserted=1)
    t.timeout(4)
    t.finish(at=0)


if __name__ == "__main__":
    print(
        compile_cycle_aware(
            build,
            name="bypass_unit",
            eager=True,
            lanes=8,
            data_width=64,
            ptag_count=256,
            ptype_count=4,
        ).emit_mlir()[:500]
    )
