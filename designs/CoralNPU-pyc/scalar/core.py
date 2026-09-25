"""Single-issue RV32I scalar core for the Coral NPU S1 slice.

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
from rvv.backend import tie_rvv
from scalar.alu import alu_result


def elaborate(m: CycleAwareCircuit, domain: CycleAwareDomain, program: tuple[int, ...]) -> None:
    """Build the S1 core. ``program`` is a tuple of encoded RV32I words."""
    if not program:
        raise ValueError("Coral NPU program must contain at least one instruction")

    pc = domain.signal(width=32, reset_value=0, name="pc")
    halted = domain.signal(width=1, reset_value=0, name="halted")
    tohost = domain.signal(width=32, reset_value=0, name="tohost")
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
    is_store = opcode == u(7, 0x23)
    is_ebreak = inst == u(32, 0x00100073)
    subtract = is_op & (funct3 == u(3, 0)) & (funct7 == u(7, 0x20))
    arith_shift = (funct7 == u(7, 0x20)) & (funct3 == u(3, 5)) & (is_op | is_opimm)

    alu_b = mux(is_op, rs2_val, imm_i)
    alu_y = alu_result(rs1_val, alu_b, funct3, subtract, arith_shift)
    link = pc + u(32, 4)
    wb = mux(
        is_lui,
        imm_u,
        mux(is_auipc, pc + imm_u, mux(is_jal | is_jalr, link, alu_y)),
    )
    writes = (is_opimm | is_op | is_lui | is_auipc | is_jal | is_jalr) & (rd != u(5, 0))

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
    next_pc = mux(
        is_jal,
        pc + imm_j,
        mux(is_jalr, jalr_target, mux(is_branch & taken, branch_target, seq_pc)),
    )

    store_addr = rs1_val + imm_s
    is_tohost = is_store & (funct3 == u(3, 2)) & (store_addr == u(32, 0))
    legal = is_opimm | is_op | is_lui | is_auipc | is_jal | is_jalr | is_branch | is_tohost | is_ebreak
    stop = past_end | is_ebreak | is_tohost | ~legal
    running = halted == u(1, 0)

    m.output("halted", wire_of(halted))
    m.output("pc", wire_of(pc))
    m.output("tohost", wire_of(tohost))
    m.output("itcm_rdata", wire_of(inst))
    m.output("dtcm_we", wire_of(is_tohost & running))
    m.output("dtcm_wdata", wire_of(rs2_val))
    tie_rvv(m)
    tie_matrix(m)

    # Next-state muxes must be built before domain.next(). Building them after
    # next() inserts an extra balance register and the write lands a cycle late.
    pc_d = mux(running & ~stop, next_pc, pc)
    halted_d = mux(running & stop, u(1, 1), halted)
    tohost_d = mux(running & is_tohost, rs2_val, tohost)
    gpr_d = [
        mux(running & writes & (rd == u(5, i)) & ~stop, wb, gpr[i]) for i in range(1, 32)
    ]

    domain.next()
    pc <<= pc_d
    halted <<= halted_d
    tohost <<= tohost_d
    for i, nxt in enumerate(gpr_d, start=1):
        gpr[i] <<= nxt


def cat_imm(*parts: object):
    """MSB-first concatenate. Local import avoids a cycle with ``pycircuit.cat``."""
    from pycircuit import cat

    return cat(*parts)


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
