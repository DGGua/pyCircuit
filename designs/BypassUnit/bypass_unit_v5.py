from __future__ import annotations

from pycircuit import (
    CycleAwareCircuit,
    CycleAwareDomain,
    Tb,
    Vec,
    cas,
    compile_cycle_aware,
    testbench,
)

PTYPE_C = 0
PTYPE_P = 1
PTYPE_T = 2
PTYPE_U = 3

CYCLE_WB  = 0   # w3: writeback stage — oldest result, available first
CYCLE_MEM = 1   # w2: memory stage
CYCLE_EX  = 2   # w1: execute stage — newest result, available last
CYCLE_ISS = 0   # source operands: issue stage


def _const_vec(m: CycleAwareCircuit, value: int, *, width: int, lanes: int) -> Vec:
    return Vec([m.const(int(value), width=int(width)) for _ in range(int(lanes))])


def _select_stage_batch(
    m: CycleAwareCircuit,
    domain: CycleAwareDomain,
    *,
    src_valid: Vec,
    src_ptag: Vec,
    src_ptype: Vec,
    lane_valid: Vec,
    lane_ptag: Vec,
    lane_ptype: Vec,
    lane_data: Vec,
    lane_nums: Vec,
    zero_lane,
    zero_data,
    lanes: int,
) -> tuple[Vec, Vec, Vec]:
    """Pick the first matching write-back lane for every source lane.

    Cycle-aware Vec operations align source and write-back lanes automatically.
    """
    _ = (m, domain)
    n = int(lanes)

    sv_bc = src_valid.broadcast(dim=1, size=n)
    sp_bc = src_ptag.broadcast(dim=1, size=n)
    st_bc = src_ptype.broadcast(dim=1, size=n)

    lv_bc = lane_valid.broadcast(dim=0, size=n)
    lp_bc = lane_ptag.broadcast(dim=0, size=n)
    lt_bc = lane_ptype.broadcast(dim=0, size=n)

    match = sv_bc & lv_bc & (lp_bc == sp_bc) & (lt_bc == st_bc)
    has = match.or_reduce(dim=1)
    sel_lane = Vec([match[i].priority_mux(lane_nums, zero=zero_lane) for i in range(n)])

    ld_bc = lane_data.broadcast(dim=0, size=n)
    sel_data = Vec([match[i].priority_mux(ld_bc[i], zero=zero_data) for i in range(n)])

    return has, sel_lane, sel_data


def _resolve_src_batch(
    m: CycleAwareCircuit,
    domain: CycleAwareDomain,
    *,
    src_valid: Vec,
    src_ptag: Vec,
    src_ptype: Vec,
    src_rf_data: Vec,
    w1_valid: Vec,
    w1_ptag: Vec,
    w1_ptype: Vec,
    w1_data: Vec,
    w2_valid: Vec,
    w2_ptag: Vec,
    w2_ptype: Vec,
    w2_data: Vec,
    w3_valid: Vec,
    w3_ptag: Vec,
    w3_ptype: Vec,
    w3_data: Vec,
    lanes: int,
    lane_w: int,
    data_w: int,
) -> tuple[Vec, Vec, Vec, Vec]:
    """Resolve all source lanes across 3 pipelined write-back stages.

    Pipeline model (auto-cycle-balanced):
      Cycle 0 — w3 (WB):  data available first, lowest priority
      Cycle 1 — w2 (MEM): overrides w3
      Cycle 2 — w1 (EX):  data available last, highest priority
    Source operands @ cycle 0 (issue stage).

    The priority chain mixes CAS signals at different cycles;
    auto-cycle-balancing inserts pipeline DFFs between stages:
      w3 result @ 0  →  DFF  →  w2 mux @ 1  →  DFF  →  w1 mux @ 2
    Output @ cycle 2.
    """
    n = int(lanes)
    lane_nums = Vec([m.const(j, width=lane_w) for j in range(n)])
    zero_data = m.const(0, width=data_w)
    zero_lane = m.const(0, width=lane_w)

    # ── Cycle 0: w3 (WB stage) — lowest priority ──
    # src @ cycle 0, w3 lanes @ cycle 0 → match results @ cycle 0
    has_w3, lane_w3, data_w3 = _select_stage_batch(
        m, domain,
        src_valid=src_valid,
        src_ptag=src_ptag,
        src_ptype=src_ptype,
        lane_valid=w3_valid,
        lane_ptag=w3_ptag,
        lane_ptype=w3_ptype,
        lane_data=w3_data,
        lane_nums=lane_nums,
        zero_lane=zero_lane,
        zero_data=zero_data,
        lanes=n,
    )

    out_data = has_w3.select(data_w3, src_rf_data)
    out_hit = has_w3.select(_const_vec(m, 1, width=1, lanes=n), _const_vec(m, 0, width=1, lanes=n))
    out_stage = has_w3.select(_const_vec(m, 3, width=2, lanes=n), _const_vec(m, 0, width=2, lanes=n))
    out_lane = has_w3.select(lane_w3, _const_vec(m, 0, width=lane_w, lanes=n))

    # ── Cycle 1: w2 (MEM stage) — overrides w3 ──
    # src @ cycle 0 combined with w2 lanes @ cycle 1 → match results @ cycle 1
    # out_* from w3 @ cycle 0 auto-delayed to cycle 1 via DFF
    has_w2, lane_w2, data_w2 = _select_stage_batch(
        m, domain,
        src_valid=src_valid,
        src_ptag=src_ptag,
        src_ptype=src_ptype,
        lane_valid=w2_valid,
        lane_ptag=w2_ptag,
        lane_ptype=w2_ptype,
        lane_data=w2_data,
        lane_nums=lane_nums,
        zero_lane=zero_lane,
        zero_data=zero_data,
        lanes=n,
    )

    out_data = has_w2.select(data_w2, out_data)
    out_hit = has_w2.select(_const_vec(m, 1, width=1, lanes=n), out_hit)
    out_stage = has_w2.select(_const_vec(m, 2, width=2, lanes=n), out_stage)
    out_lane = has_w2.select(lane_w2, out_lane)

    # ── Cycle 2: w1 (EX stage) — highest priority, overrides w2 ──
    # src @ cycle 0 combined with w1 lanes @ cycle 2 → match results @ cycle 2
    # out_* from w2 mux @ cycle 1 auto-delayed to cycle 2 via DFF
    has_w1, lane_w1, data_w1 = _select_stage_batch(
        m, domain,
        src_valid=src_valid,
        src_ptag=src_ptag,
        src_ptype=src_ptype,
        lane_valid=w1_valid,
        lane_ptag=w1_ptag,
        lane_ptype=w1_ptype,
        lane_data=w1_data,
        lane_nums=lane_nums,
        zero_lane=zero_lane,
        zero_data=zero_data,
        lanes=n,
    )

    out_data = has_w1.select(data_w1, out_data)
    out_hit = has_w1.select(_const_vec(m, 1, width=1, lanes=n), out_hit)
    out_stage = has_w1.select(_const_vec(m, 1, width=2, lanes=n), out_stage)
    out_lane = has_w1.select(lane_w1, out_lane)

    return (
        out_data,
        out_hit,
        out_stage,
        out_lane,
    )


def bypass_unit(
    m: CycleAwareCircuit,
    domain: CycleAwareDomain,
    *,
    prefix: str = "bp",
    lanes: int = 8,
    data_width: int = 64,
    ptag_count: int = 256,
    ptype_count: int = 4,
    inputs: dict[str, Vec] | None = None,
    emit_outputs: bool = True,
) -> dict[str, Vec]:
    """Pipelined bypass / forwarding network with cycle-aware pipeline stages.

    Each write-back stage is annotated at its pipeline cycle:
      w3 @ cycle 0 (WB)  — oldest result, lowest priority
      w2 @ cycle 1 (MEM)
      w1 @ cycle 2 (EX)  — newest result, highest priority
    Source operands @ cycle 0 (issue stage).

    Auto-cycle-balancing inserts DFF pipeline registers when signals from
    different cycles are combined in the priority mux chain:
      w3 result@0 → DFF → w2 mux@1 → DFF → w1 mux@2 → output@2
    """
    _in = inputs or {}
    _out: dict[str, Vec] = {}

    if lanes <= 0:
        raise ValueError("bypass_unit lanes must be > 0")
    if data_width <= 0:
        raise ValueError("bypass_unit data_width must be > 0")
    if ptag_count <= 0:
        raise ValueError("bypass_unit ptag_count must be > 0")
    if ptype_count <= PTYPE_U:
        raise ValueError("bypass_unit ptype_count must be >= 4 to represent C/P/T/U")

    ptag_w  = max(1, (ptag_count - 1).bit_length())
    ptype_w = max(1, (ptype_count - 1).bit_length())
    lane_w  = max(1, (lanes - 1).bit_length())

    def vec_input(key: str, *, width: int, cycle: int) -> Vec:
        if key in _in:
            vec = _in[key]
            if not isinstance(vec, Vec):
                raise TypeError(f"{key} override must be a Vec")
            return vec
        port = f"{prefix}_{key}"
        raw = m.input(port, width=int(width), shape=int(lanes))
        if not isinstance(raw, Vec):
            raise TypeError(f"{port} shaped input did not produce Vec")
        return Vec([cas(domain, wire, cycle=int(cycle)) for wire in raw])

    def stage_inputs(stage: str, cycle: int) -> dict[str, Vec]:
        return {
            "valid": vec_input(f"{stage}_valid", width=1, cycle=cycle),
            "ptag": vec_input(f"{stage}_ptag", width=ptag_w, cycle=cycle),
            "ptype": vec_input(f"{stage}_ptype", width=ptype_w, cycle=cycle),
            "data": vec_input(f"{stage}_data", width=data_width, cycle=cycle),
        }

    def source_inputs(src: str) -> dict[str, Vec]:
        return {
            "valid": vec_input(f"i2_{src}_valid", width=1, cycle=CYCLE_ISS),
            "ptag": vec_input(f"i2_{src}_ptag", width=ptag_w, cycle=CYCLE_ISS),
            "ptype": vec_input(f"i2_{src}_ptype", width=ptype_w, cycle=CYCLE_ISS),
            "rf_data": vec_input(f"i2_{src}_rf_data", width=data_width, cycle=CYCLE_ISS),
        }

    # ── Write-back stage inputs: each stage is one shaped Vec port. ──
    w = {
        "w3": stage_inputs("w3", CYCLE_WB),
        "w2": stage_inputs("w2", CYCLE_MEM),
        "w1": stage_inputs("w1", CYCLE_EX),
    }

    def resolve_source(src: str) -> None:
        src_v = source_inputs(src)
        out_data, out_hit, out_stage, out_lane = _resolve_src_batch(
            m, domain,
            src_valid=src_v["valid"], src_ptag=src_v["ptag"],
            src_ptype=src_v["ptype"], src_rf_data=src_v["rf_data"],
            w1_valid=w["w1"]["valid"], w1_ptag=w["w1"]["ptag"],
            w1_ptype=w["w1"]["ptype"], w1_data=w["w1"]["data"],
            w2_valid=w["w2"]["valid"], w2_ptag=w["w2"]["ptag"],
            w2_ptype=w["w2"]["ptype"], w2_data=w["w2"]["data"],
            w3_valid=w["w3"]["valid"], w3_ptag=w["w3"]["ptag"],
            w3_ptype=w["w3"]["ptype"], w3_data=w["w3"]["data"],
            lanes=lanes, lane_w=lane_w, data_w=data_width,
        )

        if emit_outputs:
            m.output(f"{prefix}_i2_{src}_data", out_data)
            m.output(f"{prefix}_i2_{src}_hit", out_hit)
            m.output(f"{prefix}_i2_{src}_sel_stage", out_stage)
            m.output(f"{prefix}_i2_{src}_sel_lane", out_lane)

        _out.update({
            f"i2_{src}_data": out_data,
            f"i2_{src}_hit": out_hit,
            f"i2_{src}_sel_stage": out_stage,
            f"i2_{src}_sel_lane": out_lane,
        })

    # ── Per-source-side batch resolution ──
    resolve_source("srcL")
    resolve_source("srcR")


    return _out


bypass_unit.__pycircuit_name__ = "bypass_unit"


@testbench
def tb(t: Tb) -> None:
    t.clock("clk")
    t.reset("rst", cycles_asserted=1, cycles_deasserted=1)
    t.timeout(4)
    t.finish(at=0)


if __name__ == "__main__":
    print(
        compile_cycle_aware(
            bypass_unit,
            name="bypass_unit",
            eager=True,
            lanes=8,
            data_width=64,
            ptag_count=256,
            ptype_count=4,
        ).emit_mlir()[:500]
    )
