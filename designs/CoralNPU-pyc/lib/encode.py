"""RV32I encoders for Coral NPU program images.

These run in Python while a test is elaborated. They are not hardware.
Encodings follow the RISC-V unprivileged ISA, which is what Coral's scalar
frontend decodes.
"""

from __future__ import annotations


def addi(rd: int, rs1: int, imm: int) -> int:
    """I-type add immediate. ``addi x0, x0, 0`` is ``nop``."""
    return _itype(imm, rs1, 0, rd, 0x13)


def lui(rd: int, imm20: int) -> int:
    return ((imm20 & 0xFFFFF) << 12) | ((rd & 0x1F) << 7) | 0x37


def auipc(rd: int, imm20: int) -> int:
    return ((imm20 & 0xFFFFF) << 12) | ((rd & 0x1F) << 7) | 0x17


def op(funct7: int, rs2: int, rs1: int, funct3: int, rd: int) -> int:
    return (
        ((funct7 & 0x7F) << 25)
        | ((rs2 & 0x1F) << 20)
        | ((rs1 & 0x1F) << 15)
        | ((funct3 & 0x7) << 12)
        | ((rd & 0x1F) << 7)
        | 0x33
    )


def add(rd: int, rs1: int, rs2: int) -> int:
    return op(0x00, rs2, rs1, 0, rd)


def sub(rd: int, rs1: int, rs2: int) -> int:
    return op(0x20, rs2, rs1, 0, rd)


def sll(rd: int, rs1: int, rs2: int) -> int:
    return op(0x00, rs2, rs1, 1, rd)


def slt(rd: int, rs1: int, rs2: int) -> int:
    return op(0x00, rs2, rs1, 2, rd)


def sltu(rd: int, rs1: int, rs2: int) -> int:
    return op(0x00, rs2, rs1, 3, rd)


def xor(rd: int, rs1: int, rs2: int) -> int:
    return op(0x00, rs2, rs1, 4, rd)


def srl(rd: int, rs1: int, rs2: int) -> int:
    return op(0x00, rs2, rs1, 5, rd)


def sra(rd: int, rs1: int, rs2: int) -> int:
    return op(0x20, rs2, rs1, 5, rd)


def or_(rd: int, rs1: int, rs2: int) -> int:
    return op(0x00, rs2, rs1, 6, rd)


def and_(rd: int, rs1: int, rs2: int) -> int:
    return op(0x00, rs2, rs1, 7, rd)


def jal(rd: int, imm: int) -> int:
    imm &= 0x1FFFFF
    return (
        (((imm >> 20) & 0x1) << 31)
        | (((imm >> 1) & 0x3FF) << 21)
        | (((imm >> 11) & 0x1) << 20)
        | (((imm >> 12) & 0xFF) << 12)
        | ((rd & 0x1F) << 7)
        | 0x6F
    )


def jalr(rd: int, rs1: int, imm: int) -> int:
    return _itype(imm, rs1, 0, rd, 0x67)


def branch(funct3: int, rs1: int, rs2: int, imm: int) -> int:
    imm &= 0x1FFF
    return (
        (((imm >> 12) & 0x1) << 31)
        | (((imm >> 5) & 0x3F) << 25)
        | ((rs2 & 0x1F) << 20)
        | ((rs1 & 0x1F) << 15)
        | ((funct3 & 0x7) << 12)
        | (((imm >> 1) & 0xF) << 8)
        | (((imm >> 11) & 0x1) << 7)
        | 0x63
    )


def beq(rs1: int, rs2: int, imm: int) -> int:
    return branch(0, rs1, rs2, imm)


def bne(rs1: int, rs2: int, imm: int) -> int:
    return branch(1, rs1, rs2, imm)


def blt(rs1: int, rs2: int, imm: int) -> int:
    return branch(4, rs1, rs2, imm)


def bge(rs1: int, rs2: int, imm: int) -> int:
    return branch(5, rs1, rs2, imm)


def lb(rd: int, rs1: int, imm: int) -> int:
    return _itype(imm, rs1, 0, rd, 0x03)


def lh(rd: int, rs1: int, imm: int) -> int:
    return _itype(imm, rs1, 1, rd, 0x03)


def lw(rd: int, rs1: int, imm: int) -> int:
    return _itype(imm, rs1, 2, rd, 0x03)


def lbu(rd: int, rs1: int, imm: int) -> int:
    return _itype(imm, rs1, 4, rd, 0x03)


def lhu(rd: int, rs1: int, imm: int) -> int:
    return _itype(imm, rs1, 5, rd, 0x03)


def sb(rs2: int, rs1: int, imm: int) -> int:
    return _stype(imm, rs2, rs1, 0)


def sh(rs2: int, rs1: int, imm: int) -> int:
    return _stype(imm, rs2, rs1, 1)


def sw(rs2: int, rs1: int, imm: int) -> int:
    """Store word. Address 0 is the mailbox (``tohost``). Other stores hit DTCM."""
    return _stype(imm, rs2, rs1, 2)


def mul(rd: int, rs1: int, rs2: int) -> int:
    return op(0x01, rs2, rs1, 0, rd)


def mulh(rd: int, rs1: int, rs2: int) -> int:
    return op(0x01, rs2, rs1, 1, rd)


def mulhsu(rd: int, rs1: int, rs2: int) -> int:
    return op(0x01, rs2, rs1, 2, rd)


def mulhu(rd: int, rs1: int, rs2: int) -> int:
    return op(0x01, rs2, rs1, 3, rd)


def div(rd: int, rs1: int, rs2: int) -> int:
    return op(0x01, rs2, rs1, 4, rd)


def divu(rd: int, rs1: int, rs2: int) -> int:
    return op(0x01, rs2, rs1, 5, rd)


def rem(rd: int, rs1: int, rs2: int) -> int:
    return op(0x01, rs2, rs1, 6, rd)


def remu(rd: int, rs1: int, rs2: int) -> int:
    return op(0x01, rs2, rs1, 7, rd)


def ebreak() -> int:
    return 0x00100073


def _stype(imm: int, rs2: int, rs1: int, funct3: int) -> int:
    imm &= 0xFFF
    return (
        (((imm >> 5) & 0x7F) << 25)
        | ((rs2 & 0x1F) << 20)
        | ((rs1 & 0x1F) << 15)
        | ((funct3 & 0x7) << 12)
        | ((imm & 0x1F) << 7)
        | 0x23
    )


def _itype(imm: int, rs1: int, funct3: int, rd: int, opcode: int) -> int:
    return (
        ((imm & 0xFFF) << 20)
        | ((rs1 & 0x1F) << 15)
        | ((funct3 & 0x7) << 12)
        | ((rd & 0x1F) << 7)
        | (opcode & 0x7F)
    )
