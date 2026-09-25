"""Decode one RV32IM instruction in a 4-wide issue packet.

The lane does not read the instruction ROM. The core registers the fetched
words first so the ROM mux tree is not stacked on the ALU.
"""

from __future__ import annotations

from pycircuit import mux, u

from scalar.alu import alu_result
from scalar.core_util import cat_imm


def decode_lane(inst, pc, gpr, word_index, program_len: int, read_gpr):
    """Return the control signals for one packet lane."""
    opcode = inst[0:7]
    rd = inst[7:12]
    funct3 = inst[12:15]
    rs1 = inst[15:20]
    rs2 = inst[20:25]
    funct7 = inst[25:32]
    rs1_val = read_gpr(gpr, rs1)
    rs2_val = read_gpr(gpr, rs2)

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
    alu_y = alu_result(rs1_val, mux(is_alu_op, rs2_val, imm_i), funct3, subtract, arith_shift)
    link = pc + u(32, 4)
    # vsetvli writes VLMAX. This slice is e32, m1, VLEN=128, so VLMAX is 4.
    funct6 = funct7[1:7]
    is_opv = opcode == u(7, 0x57)
    is_vset = is_opv & (funct3 == u(3, 7))
    is_valu = is_opv & (funct3 == u(3, 0)) & (
        (funct6 == u(6, 0)) | (funct6 == u(6, 9)) | (funct6 == u(6, 10)) | (funct6 == u(6, 11))
    )
    is_vle = (opcode == u(7, 0x07)) & (funct3 == u(3, 6))
    is_vse = (opcode == u(7, 0x27)) & (funct3 == u(3, 6))
    is_vec = is_vset | is_valu | is_vle | is_vse
    scalar_y = mux(is_vset, u(32, 4), mux(is_lui, imm_u, mux(is_auipc, pc + imm_u, mux(is_jal | is_jalr, link, alu_y))))

    eq = rs1_val == rs2_val
    lts = rs1_val.as_signed() < rs2_val.as_signed()
    ltu = rs1_val.as_unsigned() < rs2_val.as_unsigned()
    taken = mux(
        funct3 == u(3, 0),
        eq,
        mux(
            funct3 == u(3, 1),
            ~eq,
            mux(funct3 == u(3, 4), lts, mux(funct3 == u(3, 5), ~lts, mux(funct3 == u(3, 6), ltu, ~ltu))),
        ),
    )
    branch_target = pc + imm_b
    jalr_target = (rs1_val + imm_i) & u(32, 0xFFFFFFFE)
    redir = is_jal | is_jalr | (is_branch & taken)
    target = mux(is_jal, pc + imm_j, mux(is_jalr, jalr_target, branch_target))

    mem_addr = mux(is_store, rs1_val + imm_s, rs1_val + imm_i)
    in_dtcm = mem_addr[15:32] == u(17, 2)
    legal_load = is_load & in_dtcm & (
        (funct3 == u(3, 0)) | (funct3 == u(3, 1)) | (funct3 == u(3, 2)) | (funct3 == u(3, 4)) | (funct3 == u(3, 5))
    )
    legal_store = is_store & in_dtcm & ((funct3 == u(3, 0)) | (funct3 == u(3, 1)) | (funct3 == u(3, 2)))
    is_tohost = is_store & (funct3 == u(3, 2)) & (mem_addr == u(32, 0))
    # Opcode class only. Address checks stay on the memory enable so the
    # issue mask does not include the adder.
    legal = is_opimm | is_alu_op | is_m | is_lui | is_auipc | is_jal | is_jalr | is_branch | is_load | is_store | is_ebreak | is_vec
    writes_reg = (is_opimm | is_alu_op | is_lui | is_auipc | is_jal | is_jalr | legal_load | is_m | is_vset) & (rd != u(5, 0))
    writes_pack = (is_opimm | is_alu_op | is_lui | is_auipc) & (rd != u(5, 0))
    reads_rs1 = is_alu_op | is_opimm | is_m | is_branch | is_load | is_store | is_jalr
    reads_rs2 = is_alu_op | is_m | is_branch | is_store
    # Opcode-only tail. Address math stays out of this mask so four lanes
    # do not stack adders into the next-PC path. Vector ops issue alone.
    blocks = is_load | is_store | is_mul | is_divop | is_ebreak | is_jal | is_jalr | is_branch | is_vec
    pack_ok = is_opimm | is_alu_op | is_lui | is_auipc
    past_end = word_index >= u(8, program_len)
    return {
        "inst": inst,
        "rd": rd,
        "funct3": funct3,
        "rs1": rs1,
        "rs2": rs2,
        "rs1_val": rs1_val,
        "rs2_val": rs2_val,
        "is_mul": is_mul,
        "is_divop": is_divop,
        "scalar_y": scalar_y,
        "redir": redir,
        "target": target,
        "mem_addr": mem_addr,
        "legal_load": legal_load,
        "legal_store": legal_store,
        "is_tohost": is_tohost,
        "is_ebreak": is_ebreak,
        "legal": legal,
        "writes_reg": writes_reg,
        "writes_pack": writes_pack,
        "reads_rs1": reads_rs1,
        "reads_rs2": reads_rs2,
        "blocks": blocks,
        "pack_ok": pack_ok,
        "past_end": past_end,
        "signed_div": (funct3 == u(3, 4)) | (funct3 == u(3, 6)),
        "funct6": funct6,
        "is_vset": is_vset,
        "is_valu": is_valu,
        "is_vle": is_vle,
        "is_vse": is_vse,
    }
