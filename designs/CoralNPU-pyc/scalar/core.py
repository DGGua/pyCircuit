"""Four-wide RV32IM scalar core for the Coral NPU slice.

A registered fetch window holds four instruction words so the ROM mux tree is
not stacked on the ALU. The window issues a prefix of up to four instructions.
A later lane waits when it reads or writes a register an earlier lane in the
same window writes. Loads, stores, multiplies, and divides issue alone.
Branches and jumps end the window. Retirement is in lane order, so an earlier
write lands before a later one in the same cycle.

Multiply still commits on the second cycle. Divide still steps one bit per
cycle. Integer vector ops are e32, m1, VLEN=128: add and logic retire in the
issue cycle, and a unit-stride load or store moves one word per cycle.
A store word to address 0 writes ``tohost`` and stops the PC.
"""

from __future__ import annotations

from pycircuit import CycleAwareCircuit, CycleAwareDomain, mux, u, wire_of

from matrix.tiles import tie_matrix
from mem.dtcm import attach_dtcm
from rvv.alu import extract_lane, insert_lane, vv_op
from rvv.backend import tie_rvv
from scalar.core_util import cat_imm
from scalar.lane import decode_lane
from scalar.muldiv import div_step, mul_result


def elaborate(m: CycleAwareCircuit, domain: CycleAwareDomain, program: tuple[int, ...]) -> None:
    """Build the scalar core. ``program`` is a tuple of encoded instruction words."""
    if not program:
        raise ValueError("Coral NPU program must contain at least one instruction")

    pc = domain.signal(width=32, reset_value=0, name="pc")
    halted = domain.signal(width=1, reset_value=0, name="halted")
    tohost = domain.signal(width=32, reset_value=0, name="tohost")
    filled = domain.signal(width=1, reset_value=0, name="filled")
    cooked = domain.signal(width=1, reset_value=0, name="cooked")
    # 0 = issue, 1 = commit a multiply, 2 = divider busy, 3 = vector memory.
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
    win = [domain.signal(width=32, reset_value=0, name=f"win{i}") for i in range(4)]
    yreg = [domain.signal(width=32, reset_value=0, name=f"y{i}") for i in range(4)]
    tgt = domain.signal(width=32, reset_value=0, name="tgt")
    redir_r = domain.signal(width=1, reset_value=0, name="redir_r")
    gpr = [domain.signal(width=32, reset_value=0, name=f"x{i}") for i in range(32)]
    vrf = [domain.signal(width=128, reset_value=0, name=f"v{i}") for i in range(32)]
    vbeat = domain.signal(width=3, reset_value=0, name="vbeat")
    vbase = domain.signal(width=15, reset_value=0, name="vbase")
    vdst = domain.signal(width=5, reset_value=0, name="vdst")
    vload = domain.signal(width=1, reset_value=0, name="vload")
    vsrc = domain.signal(width=128, reset_value=0, name="vsrc")
    vacc = domain.signal(width=128, reset_value=0, name="vacc")

    lanes = []
    for i, inst in enumerate(win):
        lane_pc = pc + u(32, 4 * i)
        word = (pc >> 2).trunc(8) + u(8, i)
        lanes.append(decode_lane(inst, lane_pc, gpr, word, len(program), _read_gpr))
    head = lanes[0]

    issuing = phase == u(2, 0)
    running = halted == u(1, 0)
    ready = filled & cooked & issuing & running
    # Each lane's raw bit is independent. The prefix AND is applied after
    # the bits are registered, so four hazard checks do not stack.
    raw = []
    for i in range(4):
        hazard = u(1, 0)
        for j in range(i):
            earlier = lanes[j]
            this = lanes[i]
            reads = (this["reads_rs1"] & (earlier["rd"] == this["rs1"])) | (
                this["reads_rs2"] & (earlier["rd"] == this["rs2"])
            )
            waw = this["writes_pack"] & (earlier["rd"] == this["rd"])
            hazard = hazard | (earlier["writes_pack"] & (reads | waw))
        if i == 0:
            raw.append(~lanes[i]["past_end"] & lanes[i]["legal"])
        else:
            raw.append(~lanes[i]["past_end"] & lanes[i]["pack_ok"] & ~hazard & ~lanes[i - 1]["blocks"])
    can_r = [domain.signal(width=1, reset_value=0, name=f"can{i}") for i in range(4)]
    issue = [ready & can_r[0]]
    for i in range(1, 4):
        issue.append(issue[i - 1] & can_r[i])

    # Only lane 0 can be a memory, multiply, or divide op. Later lanes are blocked.
    mem_addr = head["mem_addr"]
    rs2_val = head["rs2_val"]
    rs1_val = head["rs1_val"]
    funct3 = head["funct3"]
    rd = head["rd"]
    store_w = head["legal_store"] & issue[0]
    scalar_strb = mux(funct3 == u(3, 0), u(4, 0x1), mux(funct3 == u(3, 1), u(4, 0x3), u(4, 0xF))).zext(8)
    vec_mem = phase == u(2, 3)
    beat_off = cat_imm(u(10, 0), vbeat, u(2, 0))
    mem_off = mux(vec_mem, vbase + beat_off, mem_addr[0:15])
    mem_we = mux(vec_mem, ~vload, store_w)
    mem_data = mux(vec_mem, extract_lane(vsrc, vbeat).zext(64), rs2_val.zext(64))
    mem_strb = mux(vec_mem, u(8, 0xF), scalar_strb)
    rdata = attach_dtcm(m, domain, mem_off, mem_we, mem_off, mem_data, mem_strb)[0:32]
    narrow = mux(funct3[0:1], rdata[0:16].sext(width=32), rdata[0:8].sext(width=32))
    wide = mux(funct3[0:1], rdata[0:16].zext(width=32), rdata[0:8].zext(width=32))
    load_val = mux(funct3 == u(3, 2), rdata, mux(funct3[2:3], wide, narrow))

    step_q, step_r = div_step(div_quot, div_rem, div_den)
    div_done = (phase == u(2, 2)) & (div_count == u(6, 32))
    raw_q = mux(div_by0, u(32, 0xFFFFFFFF), mux(div_neg_q, (~div_quot) + u(32, 1), div_quot))
    raw_r = mux(div_by0, div_orig, mux(div_neg_r, (~div_rem) + u(32, 1), div_rem))
    div_y = mux(div_is_div, raw_q, raw_r)
    wb0 = mux(
        (phase == u(2, 1)) | div_done,
        mux(phase == u(2, 1), mul_result(rs1_val, rs2_val, funct3), div_y),
        mux(head["legal_load"], load_val, yreg[0]),
    )
    writes0 = (
        (issue[0] & head["writes_reg"] & ~head["is_mul"] & ~head["is_divop"])
        | ((phase == u(2, 1)) | div_done) & (rd != u(5, 0))
    )

    seq = mux(issue[3], pc + u(32, 16), mux(issue[2], pc + u(32, 12), mux(issue[1], pc + u(32, 8), pc + u(32, 4))))
    # Only lane 0 can redirect: a branch or jump ends the packet.
    pc_now = mux(issue[0] & redir_r, tgt, seq)
    start_vmem = issue[0] & (head["is_vle"] | head["is_vse"])
    hold = (issue[0] & (head["is_mul"] | head["is_divop"])) | ((phase == u(2, 2)) & ~div_done) | start_vmem | vec_mem
    is_mem_op = (head["inst"][0:7] == u(7, 0x03)) | (head["inst"][0:7] == u(7, 0x23))
    bad_mem = issue[0] & is_mem_op & ~head["legal_load"] & ~head["legal_store"] & ~head["is_tohost"]
    stop = ready & (head["past_end"] | ~head["legal"] | (issue[0] & (head["is_ebreak"] | head["is_tohost"])) | bad_mem)
    # Fetch does not wait on the address-fault comparator. Halt still does.
    advance = ready & issue[0] & ~hold & ~head["past_end"] & head["legal"] & ~(issue[0] & (head["is_ebreak"] | head["is_tohost"]))

    m.output("halted", wire_of(halted))
    m.output("pc", wire_of(pc))
    m.output("tohost", wire_of(tohost))
    m.output("itcm_rdata", wire_of(head["inst"]))
    m.output("dtcm_we", wire_of(store_w))
    m.output("dtcm_wdata", wire_of(rs2_val))
    tie_rvv(m, ~vec_mem)
    tie_matrix(m)

    # Next-state muxes must be built before domain.next().
    finish_md = running & ((phase == u(2, 1)) | div_done)
    finish_vec = running & vec_mem & (vbeat == u(3, 3))
    finish_busy = finish_md | finish_vec
    pc_d = mux(finish_busy, pc + u(32, 4), mux(advance, pc_now, pc))
    halted_d = mux(stop, u(1, 1), halted)
    tohost_d = mux(issue[0] & head["is_tohost"], rs2_val, tohost)
    phase_d = mux(
        ~running | stop,
        phase,
        mux(
            issue[0] & head["is_mul"],
            u(2, 1),
            mux(
                issue[0] & head["is_divop"],
                u(2, 2),
                mux(
                    start_vmem,
                    u(2, 3),
                    mux((phase == u(2, 1)) | div_done | finish_vec, u(2, 0), phase),
                ),
            ),
        ),
    )
    start_div = issue[0] & head["is_divop"]
    stepping = (phase == u(2, 2)) & ~div_done & running
    neg_a = head["signed_div"] & rs1_val[31:32]
    neg_b = head["signed_div"] & rs2_val[31:32]
    by0 = rs2_val == u(32, 0)
    div_count_d = mux(start_div, u(6, 0), mux(stepping, div_count + u(6, 1), mux(div_done, u(6, 0), div_count)))
    div_quot_d = mux(start_div, mux(neg_a, (~rs1_val) + u(32, 1), rs1_val), mux(stepping, step_q, div_quot))
    div_rem_d = mux(start_div, u(32, 0), mux(stepping, step_r, div_rem))
    div_den_d = mux(start_div, mux(neg_b, (~rs2_val) + u(32, 1), rs2_val), div_den)
    div_neg_q_d = mux(start_div, (neg_a ^ neg_b) & ~by0, div_neg_q)
    div_neg_r_d = mux(start_div, neg_a & ~by0, div_neg_r)
    div_by0_d = mux(start_div, by0, div_by0)
    div_is_div_d = mux(start_div, (funct3 == u(3, 4)) | (funct3 == u(3, 5)), div_is_div)
    div_orig_d = mux(start_div, rs1_val, div_orig)

    start_valu = issue[0] & head["is_valu"]
    vnext = insert_lane(vacc, rdata, vbeat)
    vbeat_d = mux(start_vmem, u(3, 0), mux(vec_mem & ~finish_vec, vbeat + u(3, 1), vbeat))
    vbase_d = mux(start_vmem, rs1_val[0:15], vbase)
    vdst_d = mux(start_vmem, rd, vdst)
    vload_d = mux(start_vmem, head["is_vle"], vload)
    vsrc_d = mux(start_vmem, _read_vrf(vrf, rd), vsrc)
    vacc_d = mux(vec_mem & vload, vnext, vacc)
    valu_y = vv_op(_read_vrf(vrf, head["rs1"]), _read_vrf(vrf, head["rs2"]), head["funct6"])

    cooking = filled & ~cooked & issuing & running & ~hold
    refill_pc = mux(finish_busy, pc + u(32, 4), mux(advance, pc_now, pc))
    refill = (~filled) | advance | finish_busy
    win_d = []
    yreg_d = []
    for i in range(4):
        fetched = _rom((refill_pc >> 2).trunc(8) + u(8, i), program)
        win_d.append(mux(refill, fetched, win[i]))
        yreg_d.append(mux(cooking, lanes[i]["scalar_y"], yreg[i]))
    tgt_d = mux(cooking, head["target"], tgt)
    redir_d = mux(cooking, head["redir"], redir_r)
    filled_d = u(1, 1)
    cooked_d = mux(cooking, u(1, 1), mux(refill, u(1, 0), cooked))

    gpr_d = []
    for reg in range(1, 32):
        nxt = mux(running & writes0 & (rd == u(5, reg)), wb0, gpr[reg])
        for i in range(1, 4):
            lane = lanes[i]
            nxt = mux(issue[i] & lane["writes_pack"] & (lane["rd"] == u(5, reg)), yreg[i], nxt)
        gpr_d.append(nxt)
    vrf_d = []
    for reg in range(32):
        loaded = finish_vec & vload & (vdst == u(5, reg))
        nxt = mux(loaded, vnext, vrf[reg])
        nxt = mux(start_valu & (rd == u(5, reg)), valu_y, nxt)
        vrf_d.append(nxt)

    domain.next()
    pc <<= pc_d
    halted <<= halted_d
    tohost <<= tohost_d
    filled <<= filled_d
    cooked <<= cooked_d
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
    for i, nxt in enumerate(win_d):
        win[i] <<= nxt
        yreg[i] <<= yreg_d[i]
        can_r[i] <<= raw[i]
    tgt <<= tgt_d
    redir_r <<= redir_d
    for i, nxt in enumerate(gpr_d, start=1):
        gpr[i] <<= nxt
    vbeat <<= vbeat_d
    vbase <<= vbase_d
    vdst <<= vdst_d
    vload <<= vload_d
    vsrc <<= vsrc_d
    vacc <<= vacc_d
    for i, nxt in enumerate(vrf_d):
        vrf[i] <<= nxt


def _read_vrf(vrf: list, idx) -> object:
    """Binary tree, same shape as the GPR read, over 128-bit vector registers."""
    level = list(vrf)
    for bit in range(5):
        sel = idx[bit : bit + 1]
        level = [mux(sel, level[i + 1], level[i]) for i in range(0, len(level), 2)]
    return level[0]


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
