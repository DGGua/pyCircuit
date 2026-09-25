"""Single-issue RV32IM scalar core for the Coral NPU S1/S2 slice.

Fetch, decode, and execute happen in one cycle. Register and PC updates commit
on the following clock, matching pyCircuit's ``domain.next()`` rule. A store
word to address 0 writes ``tohost`` and stops the PC. ``ebreak`` stops the PC
without a register write. x0 stays zero.

This is not Coral's 4-wide scoreboard. That frontend is a later stage. Vector
and matrix instructions are not decoded; an unrecognized opcode freezes the PC
so a bad image cannot walk off the instruction ROM.
"""

from __future__ import annotations

from pycircuit import CycleAwareCircuit, CycleAwareDomain, mux, u, wire_of

from matrix.tiles import tie_matrix
from mem.dtcm import attach_dtcm
from rvv.backend import tie_rvv
from scalar.alu import alu_result
from scalar.core_util import cat_imm
from scalar.muldiv import div_step, mul_result


def elaborate(m: CycleAwareCircuit, domain: CycleAwareDomain, program: tuple[int, ...]) -> None:
    """Build the S1 core. ``program`` is a tuple of encoded RV32I words."""
    if not program:
        raise ValueError("Coral NPU program must contain at least one instruction")

    pc = domain.signal(width=32, reset_value=0, name="pc")
    halted = domain.signal(width=1, reset_value=0, name="halted")
    tohost = domain.signal(width=32, reset_value=0, name="tohost")
    # 0 = issue, 1 = commit a multiply, 2 = divider busy.
    phase = domain.signal(width=2, reset_value=0, name="phase")
    div_count = domain.signal(width=6, reset_value=0, name="div_count")
    div_quot = domain.signal(width=32, reset_value=0, name="div_quot")
    div_rem = domain.signal(width=32, reset_value=0, name="div_rem")
    div_den = domain.signal(width=32, reset_value=0, name="div_den")
    div_neg_q = domain.signal(width=1, reset_value=0, name="div_neg_q")
    div_neg_r = domain.signal(width=1, reset_value=0, name="div_neg_r")
    div_by0 = domain.signal(width=1, reset_value=0, name="div_by0")
    div_is_div = domain.signal(width=1, reset_value=0, name="div_is_div")
    div_orig = domain.signal(width=32, reset_value=0, name="div_orig")
    gpr = [domain.signal(width=32, reset_value=0, name=f"x{i}") for i in range(32)]

    idx = (pc >> 2).trunc(8)
    inst = _rom(idx, program)
    past_end = idx >= u(8, len(program))

    opcode = inst[0:7]
    rd = inst[7:12]
    funct3 = inst[12:15]
    rs1 = inst[15:20]
    rs2 = inst[20:25]
    funct7 = inst[25:32]
    rs1_val = _read_gpr(gpr, rs1)
    rs2_val = _read_gpr(gpr, rs2)

    imm_i = inst[20:32].sext(32)
    imm_s = cat_imm(inst[25:32], inst[7:12]).sext(32)
    imm_b = cat_imm(inst[31:32], inst[7:8], inst[25:31], inst[8:12], u(1, 0)).sext(32)
    imm_u = cat_imm(inst[12:32], u(12, 0))
    imm_j = cat_imm(inst[31:32], inst[12:20], inst[20:21], inst[21:31], u(1, 0)).sext(32)

    is_opimm = opcode == u(7, 0x13)
    is_lui = opcode == u(7, 0x37)
    is_auipc = opcode == u(7, 0x17)
    is_op = opcode == u(7, 0x33)
    is_jal = opcode == u(7, 0x6F)
    is_jalr = opcode == u(7, 0x67)
    is_branch = opcode == u(7, 0x63)
    is_load = opcode == u(7, 0x03)
    is_store = opcode == u(7, 0x23)
    is_ebreak = inst == u(32, 0x00100073)
    is_m = is_op & (funct7 == u(7, 0x01))
    is_alu_op = is_op & ~is_m
    is_mul = is_m & (funct3[2:3] == u(1, 0))
    is_divop = is_m & (funct3[2:3] == u(1, 1))
    subtract = is_alu_op & (funct3 == u(3, 0)) & (funct7 == u(7, 0x20))
    arith_shift = (funct7 == u(7, 0x20)) & (funct3 == u(3, 5)) & (is_alu_op | is_opimm)

    alu_b = mux(is_alu_op, rs2_val, imm_i)
    alu_y = alu_result(rs1_val, alu_b, funct3, subtract, arith_shift)
    link = pc + u(32, 4)

    eq = rs1_val == rs2_val
    lts = rs1_val.as_signed() < rs2_val.as_signed()
    ltu = rs1_val.as_unsigned() < rs2_val.as_unsigned()
    taken = mux(
        funct3 == u(3, 0),
        eq,
        mux(
            funct3 == u(3, 1),
            ~eq,
            mux(
                funct3 == u(3, 4),
                lts,
                mux(funct3 == u(3, 5), ~lts, mux(funct3 == u(3, 6), ltu, ~ltu)),
            ),
        ),
    )
    branch_target = pc + imm_b
    jalr_target = (rs1_val + imm_i) & u(32, 0xFFFFFFFE)
    seq_pc = pc + u(32, 4)
    taken_target = mux(is_branch & taken, branch_target, seq_pc)
    jump_target = mux(is_jal, pc + imm_j, jalr_target)
    redirect = mux(is_jal | is_jalr, jump_target, taken_target)

    mem_addr = mux(is_store, rs1_val + imm_s, rs1_val + imm_i)
    in_dtcm = mem_addr[15:32] == u(17, 2)
    legal_load = is_load & in_dtcm & (
        (funct3 == u(3, 0))
        | (funct3 == u(3, 1))
        | (funct3 == u(3, 2))
        | (funct3 == u(3, 4))
        | (funct3 == u(3, 5))
    )
    legal_store = is_store & in_dtcm & ((funct3 == u(3, 0)) | (funct3 == u(3, 1)) | (funct3 == u(3, 2)))
    is_tohost = is_store & (funct3 == u(3, 2)) & (mem_addr == u(32, 0))
    issuing = phase == u(2, 0)
    legal = (
        is_opimm
        | is_alu_op
        | is_m
        | is_lui
        | is_auipc
        | is_jal
        | is_jalr
        | is_branch
        | is_tohost
        | legal_load
        | legal_store
        | is_ebreak
    )
    stop = issuing & (past_end | is_ebreak | is_tohost | ~legal)
    running = halted == u(1, 0)

    dtcm_off = mem_addr[0:15]
    store_w = legal_store & issuing & running
    wstrb = mux(funct3 == u(3, 0), u(4, 0x1), mux(funct3 == u(3, 1), u(4, 0x3), u(4, 0xF))).zext(8)
    rdata = attach_dtcm(m, domain, dtcm_off, store_w, dtcm_off, rs2_val.zext(64), wstrb)
    rdata = rdata[0:32]
    # Two-level extend so a 32-entry ROM still fits the logic-depth limit.
    narrow = mux(funct3[0:1], rdata[0:16].sext(width=32), rdata[0:8].sext(width=32))
    wide = mux(funct3[0:1], rdata[0:16].zext(width=32), rdata[0:8].zext(width=32))
    load_val = mux(funct3 == u(3, 2), rdata, mux(funct3[2:3], wide, narrow))

    product = mul_result(rs1_val, rs2_val, funct3)
    signed_div = (funct3 == u(3, 4)) | (funct3 == u(3, 6))
    neg_a = signed_div & rs1_val[31:32]
    neg_b = signed_div & rs2_val[31:32]
    mag_a = mux(neg_a, (~rs1_val) + u(32, 1), rs1_val)
    mag_b = mux(neg_b, (~rs2_val) + u(32, 1), rs2_val)
    by0 = rs2_val == u(32, 0)
    step_q, step_r = div_step(div_quot, div_rem, div_den)
    div_done = (phase == u(2, 2)) & (div_count == u(6, 32))
    raw_q = mux(div_by0, u(32, 0xFFFFFFFF), mux(div_neg_q, (~div_quot) + u(32, 1), div_quot))
    raw_r = mux(div_by0, div_orig, mux(div_neg_r, (~div_rem) + u(32, 1), div_rem))
    div_y = mux(div_is_div, raw_q, raw_r)
    scalar_y = mux(is_lui, imm_u, mux(is_auipc, pc + imm_u, mux(is_jal | is_jalr, link, alu_y)))
    mem_or_scalar = mux(legal_load, load_val, scalar_y)
    mul_or_div = mux(phase == u(2, 1), product, div_y)
    wb = mux((phase == u(2, 1)) | div_done, mul_or_div, mem_or_scalar)
    scalar_write = issuing & (is_opimm | is_alu_op | is_lui | is_auipc | is_jal | is_jalr | legal_load)
    writes = (scalar_write | (phase == u(2, 1)) | div_done) & (rd != u(5, 0))
    hold = (issuing & (is_mul | is_divop)) | ((phase == u(2, 2)) & ~div_done)
    next_pc = mux(hold, pc, redirect)
    advance = running & ~stop

    m.output("halted", wire_of(halted))
    m.output("pc", wire_of(pc))
    m.output("tohost", wire_of(tohost))
    m.output("itcm_rdata", wire_of(inst))
    m.output("dtcm_we", wire_of(store_w))
    m.output("dtcm_wdata", wire_of(rs2_val))
    tie_rvv(m)
    tie_matrix(m)

    # Next-state muxes must be built before domain.next(). Building them after
    # next() inserts an extra balance register and the write lands a cycle late.
    pc_d = mux(advance, next_pc, pc)
    halted_d = mux(running & stop, u(1, 1), halted)
    tohost_d = mux(running & issuing & is_tohost, rs2_val, tohost)
    phase_d = mux(
        ~running | stop,
        phase,
        mux(
            issuing & is_mul,
            u(2, 1),
            mux(
                issuing & is_divop,
                u(2, 2),
                mux((phase == u(2, 1)) | div_done, u(2, 0), phase),
            ),
        ),
    )
    start_div = issuing & is_divop & running & ~stop
    stepping = (phase == u(2, 2)) & ~div_done & running
    div_count_d = mux(
        start_div,
        u(6, 0),
        mux(stepping, div_count + u(6, 1), mux(div_done, u(6, 0), div_count)),
    )
    div_quot_d = mux(start_div, mag_a, mux(stepping, step_q, div_quot))
    div_rem_d = mux(start_div, u(32, 0), mux(stepping, step_r, div_rem))
    div_den_d = mux(start_div, mag_b, div_den)
    div_neg_q_d = mux(start_div, (neg_a ^ neg_b) & ~by0, div_neg_q)
    div_neg_r_d = mux(start_div, neg_a & ~by0, div_neg_r)
    div_by0_d = mux(start_div, by0, div_by0)
    div_is_div_d = mux(start_div, (funct3 == u(3, 4)) | (funct3 == u(3, 5)), div_is_div)
    div_orig_d = mux(start_div, rs1_val, div_orig)
    gpr_d = [mux(running & writes & (rd == u(5, i)), wb, gpr[i]) for i in range(1, 32)]

    domain.next()
    pc <<= pc_d
    halted <<= halted_d
    tohost <<= tohost_d
    phase <<= phase_d
    div_count <<= div_count_d
    div_quot <<= div_quot_d
    div_rem <<= div_rem_d
    div_den <<= div_den_d
    div_neg_q <<= div_neg_q_d
    div_neg_r <<= div_neg_r_d
    div_by0 <<= div_by0_d
    div_is_div <<= div_is_div_d
    div_orig <<= div_orig_d
    for i, nxt in enumerate(gpr_d, start=1):
        gpr[i] <<= nxt


def _read_gpr(gpr: list, idx) -> object:
    """Binary tree on index bits. A linear 32-way mux exceeds the logic-depth limit."""
    level = list(gpr)
    for bit in range(5):
        sel = idx[bit : bit + 1]
        level = [mux(sel, level[i + 1], level[i]) for i in range(0, len(level), 2)]
    return level[0]


def _rom(idx, words: tuple[int, ...]):
    """Power-of-two instruction ROM indexed by a binary tree, same reason as the GPR."""
    size = 1
    while size < len(words):
        size *= 2
    padded = [int(w) & 0xFFFFFFFF for w in words] + [0] * (size - len(words))
    level = [u(32, w) for w in padded]
    bits = size.bit_length() - 1
    for bit in range(bits):
        sel = idx[bit : bit + 1]
        level = [mux(sel, level[i + 1], level[i]) for i in range(0, len(level), 2)]
    return level[0]
