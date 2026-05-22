"""Architecture diagrams for the unified Top-K module (runtime fmt + K).

Renders PNGs into ``designs/examples/topk/figures/``:

  1. ``topk_overview.png``        — top-level dataflow (one path, runtime fmt+K)
  2. ``stage_a_full_p_sort.png``  — Stage A as a single P-wide bitonic sort
  3. ``merge_2p_full_cell.png``   — bitonic_merge_2p_full internal wiring (P=4)
  4. ``stage_b_unified_fsm.png``  — Stage B FSM + SRAM + init_done + carry roll
  5. ``walkthrough_k4.png``       — K=4 numerical walkthrough (still valid)
  6. ``topk_all.png``             — multi-panel "everything" figure

Run:
    python designs/examples/topk/figures/draw_arch.py
"""
from __future__ import annotations

import math
from pathlib import Path

import matplotlib.pyplot as plt
from matplotlib.patches import FancyArrowPatch, FancyBboxPatch, Rectangle, Circle

HERE = Path(__file__).resolve().parent


# ── Palette ───────────────────────────────────────────────────────
C_STAGE_A   = "#2E86AB"     # blue
C_STAGE_B   = "#A23B72"     # purple
C_RUN_REG   = "#F18F01"     # orange
C_CHUNK_IN  = "#C73E1D"     # red
C_OUTPUT    = "#3B7A57"     # green
C_SRAM      = "#7E6B8F"     # muted purple
C_CARRY     = "#E26D5C"     # coral
C_FSM       = "#1B4965"     # dark teal
C_CMP       = "#5BC0BE"     # teal
C_INIT      = "#B388EB"     # lavender (init_done)
C_LAYER_BG  = "#F5F5F5"
C_EDGE      = "#222222"
C_LABEL     = "#1A1A1A"

plt.rcParams.update({
    "font.family": "DejaVu Sans",
    "font.size": 10,
    "axes.titlesize": 13,
    "axes.titleweight": "bold",
    "axes.edgecolor": "#444",
})


# ═════════════════════════════════════════════════════════════════
# Drawing primitives
# ═════════════════════════════════════════════════════════════════

def _box(ax, x, y, w, h, text, *, fc, ec=C_EDGE, fs=10, weight="normal", lw=1.5):
    patch = FancyBboxPatch(
        (x, y), w, h,
        boxstyle="round,pad=0.02,rounding_size=0.12",
        linewidth=lw, facecolor=fc, edgecolor=ec,
    )
    ax.add_patch(patch)
    ax.text(x + w / 2, y + h / 2, text,
            ha="center", va="center", color=C_LABEL, fontsize=fs, weight=weight)


def _arrow(ax, x0, y0, x1, y1, *, color=C_EDGE, lw=1.5, style="->", text=None,
           text_offset=(0.05, 0.10)):
    arr = FancyArrowPatch((x0, y0), (x1, y1),
                          arrowstyle=style, mutation_scale=14,
                          linewidth=lw, color=color)
    ax.add_patch(arr)
    if text:
        ax.text((x0 + x1) / 2 + text_offset[0],
                (y0 + y1) / 2 + text_offset[1],
                text, fontsize=8, color=color, ha="left", va="center")


def _label(ax, x, y, text, *, fs=10, color=C_LABEL, weight="normal", ha="center", va="center"):
    ax.text(x, y, text, fontsize=fs, color=color, weight=weight, ha=ha, va=va)


# ═════════════════════════════════════════════════════════════════
# 1) Top-level overview (single unified path)
# ═════════════════════════════════════════════════════════════════

def draw_overview(ax) -> None:
    ax.set_xlim(0, 16)
    ax.set_ylim(0, 13)
    ax.set_aspect("equal")
    ax.axis("off")
    ax.set_title("Unified Top-K module - one HW, runtime fmt + runtime K")

    # input chunk
    _box(ax, 0.5, 11.4, 6.3, 1.2,
         "chunk_vals[P*32]   chunk_idxs[P*idx_w]\n"
         "valid_in   fmt_sel[2]   k_in   drain_addr",
         fc=C_CHUNK_IN, fs=10, weight="bold")

    # Issue throttle
    _box(ax, 8.5, 11.4, 6.5, 1.2,
         "Issue throttle (top-level)\n"
         "rows_used = ceil(k_in / P)\n"
         "gate valid_in so spacing >= rows_used+1 cy",
         fc=C_FSM, fs=9, weight="bold")

    # Stage A
    _box(ax, 0.5, 8.0, 14.5, 2.4,
         "Stage A : full P-element bitonic sort (W = P)\n"
         "fully pipelined, log2(P)*(log2(P)+1)/2 layers x P/2 cmp_swap per layer\n"
         "P=256: 36 layers x 128 cells = 4608 cmp_swap.  cmp_swap.fmt_sel routed everywhere.",
         fc=C_STAGE_A, fs=10, weight="bold")
    _label(ax, 7.7, 7.5, "P lanes desc-sorted per cycle  +  pipelined valid",
           fs=9, weight="bold")

    # Stage B
    _box(ax, 0.5, 4.4, 14.5, 2.7,
         "Stage B : SRAM streaming running merge (always streaming)\n"
         "FSM: IDLE -> MERGE_LOOP[rows_used cy] -> IDLE\n"
         "1R1W SRAM (K_MAX/P rows x P*(32+idx_w))   +   carry reg (P lanes)\n"
         "bitonic_merge_2p_full(P): log2(2P) layers x P cmp_swap = log2(2P)*P cells\n"
         "init_done[K_MAX/P] bit vec: uninitialized row -> neg_inf(fmt_sel) at read mux",
         fc=C_STAGE_B, fs=10, weight="bold")

    # Output
    _box(ax, 1.0, 1.6, 14.0, 1.7,
         "topk_vals[P*32]    topk_idxs[P*idx_w]    running_valid    ready_out\n"
         "P-lane row addressed by drain_addr (init-done mux'd, returns -inf for unwritten rows)",
         fc=C_OUTPUT, fs=10, weight="bold")

    # Arrows
    _arrow(ax, 3.5, 11.4, 3.5, 10.4)
    _arrow(ax, 11.75, 11.4, 11.75, 10.4, text="gate", text_offset=(0.1, 0.0))
    _arrow(ax, 7.7, 8.0, 7.7, 7.1)
    _arrow(ax, 7.7, 4.4, 7.7, 3.3)


# ═════════════════════════════════════════════════════════════════
# 2) Stage A: full P-wide bitonic sort
# ═════════════════════════════════════════════════════════════════

def draw_stage_a_full(ax) -> None:
    """Show a P=16 bitonic-sort network with layer count, no heap merge tree."""
    P = 16
    layers = int(math.log2(P) * (math.log2(P) + 1) // 2)  # = 10 for P=16
    ax.set_xlim(-1, 18)
    ax.set_ylim(-1, 12)
    ax.set_aspect("equal")
    ax.axis("off")
    ax.set_title(f"Stage A - full P-bitonic sort (illustration P={P}; "
                 f"production P=256 -> 36 layers x 128 cmp_swap = 4608 cells)")

    lane_spacing = 1.0
    y_top = 10.5

    # Input lanes
    for i in range(P):
        x = i * lane_spacing + 0.5
        _box(ax, x - 0.4, y_top - 0.4, 0.8, 0.6, f"{i}",
             fc="#FFFFFF", ec="#888", fs=8)
    _label(ax, P / 2 * lane_spacing - 0.5, y_top + 0.6,
           "chunk_v[i], chunk_i[i] (P lanes per cycle)",
           fs=10, color=C_CHUNK_IN, weight="bold")

    # Stack of layer bands
    y_start = 9.4
    band_h = 0.65
    for li in range(layers):
        y = y_start - li * band_h
        ax.add_patch(Rectangle((0.0, y - 0.3), P * lane_spacing, band_h * 0.85,
                               facecolor=C_LAYER_BG, edgecolor="#CCC", zorder=0))
        _label(ax, -0.5, y + 0.05, f"L{li}", fs=9, ha="right", color="#444", weight="bold")
        # Sprinkle a few cmp_swap dots per layer for visual cue
        for i in range(0, P, 2):
            cx = i * lane_spacing + lane_spacing
            ax.add_patch(Circle((cx, y + 0.05), 0.18,
                                facecolor=C_CMP, edgecolor="#222", lw=0.8, zorder=2))

    # Output lanes
    y_out = y_start - layers * band_h - 0.45
    for i in range(P):
        x = i * lane_spacing + 0.5
        _box(ax, x - 0.4, y_out, 0.8, 0.6, f"o{i}",
             fc=C_OUTPUT, ec="#444", fs=8, weight="bold")
    _label(ax, P / 2 * lane_spacing - 0.5, y_out - 0.7,
           "out[0..P-1] desc-sorted  (forwarded to Stage B)",
           fs=10, color=C_OUTPUT, weight="bold")

    # Side annotation
    _label(ax, P * lane_spacing + 1.0, y_top - 0.5,
           "Each cmp_swap:\n"
           " - val width 32 (fp32 max)\n"
           " - bf16/fp16 in bits[15:0]\n"
           " - fp_lt 3 parallel\n"
           "   monotone-key paths\n"
           "   mux'd by fmt_sel[2]\n\n"
           f"Layers: {layers} (= P=16)\n"
           "Pipelined: 1 stage reg / layer\n"
           "Cells/layer = P/2",
           fs=9, ha="left", color="#222", weight="normal")


# ═════════════════════════════════════════════════════════════════
# 3) bitonic_merge_2p_full cell (P=4)
# ═════════════════════════════════════════════════════════════════

def draw_merge_2p_full(ax) -> None:
    """P=4 full 2P->2P merge cell with valley wiring; all lanes kept."""
    P = 4
    ax.set_xlim(-1, 14)
    ax.set_ylim(0, 11)
    ax.set_aspect("equal")
    ax.axis("off")
    ax.set_title(f"bitonic_merge_2p_full cell  (P={P}, log2(2P)={int(math.log2(2*P))} layers)\n"
                 f"Stage B uses this every cycle in MERGE_LOOP (combinational)")

    lane_spacing = 1.3
    x_offset = 1.0
    y_top = 10.0

    def lane_x(i):
        return x_offset + i * lane_spacing

    # Row 0: input A (from SRAM row) and B (carry)
    for i in range(P):
        _box(ax, lane_x(i) - 0.5, y_top - 0.4, 1.0, 0.65, f"SRAM[{i}]",
             fc=C_SRAM, fs=9, weight="bold")
    for i in range(P):
        _box(ax, lane_x(P + i) - 0.5, y_top - 0.4, 1.0, 0.65, f"carry[{i}]",
             fc=C_CARRY, fs=9, weight="bold")

    # Wiring (reverse B for valley bitonic)
    y_wire = y_top - 1.1
    for i in range(P):
        ax.plot([lane_x(i), lane_x(i)], [y_top - 0.4, y_wire],
                color=C_SRAM, lw=1.2)
    for i in range(P):
        ax.plot([lane_x(P + i), lane_x(2 * P - 1 - i)],
                [y_top - 0.4, y_wire], color=C_CARRY, lw=1.2, linestyle="--")
    _label(ax, lane_x(P + 1.5), y_wire + 0.5,
           "valley-bitonic wiring: reverse(carry)",
           fs=9, color="#555", ha="center")

    # cmp_swap layers
    n_layers = int(math.log2(2 * P))
    cmp_y_base = y_wire - 0.8
    strides = [(2 * P) >> (level + 1) for level in range(n_layers)]
    layer_h = 1.4
    for li, stride in enumerate(strides):
        cy = cmp_y_base - li * layer_h
        ax.add_patch(Rectangle((-0.5, cy - 0.5), 13.0, 1.05,
                               facecolor=C_LAYER_BG, edgecolor="none", zorder=0))
        _label(ax, -0.7, cy, f"L{li}\nstride={stride}",
               fs=9, color="#444", ha="right", weight="bold")
        pairs = [(i, i + stride) for i in range(2 * P) if (i & stride) == 0]
        for (lo, hi) in pairs:
            mid_x = (lane_x(lo) + lane_x(hi)) / 2
            _box(ax, mid_x - 0.42, cy - 0.27, 0.84, 0.55, "CMP",
                 fc=C_CMP, fs=7, weight="bold")
            ax.plot([lane_x(lo), mid_x - 0.42], [cy + 0.82, cy + 0.27], color="#333", lw=0.9)
            ax.plot([lane_x(hi), mid_x + 0.42], [cy + 0.82, cy + 0.27], color="#333", lw=0.9)
            ax.plot([mid_x - 0.42, lane_x(lo)], [cy - 0.27, cy - 0.82], color="#333", lw=0.9)
            ax.plot([mid_x + 0.42, lane_x(hi)], [cy - 0.27, cy - 0.82], color="#333", lw=0.9)

    # Output row: top P (write back to SRAM) + bottom P (next carry)
    y_out = cmp_y_base - n_layers * layer_h
    for i in range(P):
        _box(ax, lane_x(i) - 0.5, y_out - 0.4, 1.0, 0.65, f"top[{i}]",
             fc=C_OUTPUT, fs=9, weight="bold")
    for i in range(P, 2 * P):
        _box(ax, lane_x(i) - 0.5, y_out - 0.4, 1.0, 0.65, f"bot[{i-P}]",
             fc=C_CARRY, fs=9, weight="bold")
    _label(ax, lane_x(P / 2 - 0.5), y_out - 1.1,
           "top P -> SRAM[r] (writeback)",
           fs=9, color=C_OUTPUT, weight="bold")
    _label(ax, lane_x(P * 1.5 - 0.5), y_out - 1.1,
           "bottom P -> carry (next iteration)",
           fs=9, color=C_CARRY, weight="bold")


# ═════════════════════════════════════════════════════════════════
# 4) Stage B FSM + init_done + SRAM + carry roll
# ═════════════════════════════════════════════════════════════════

def draw_stage_b_unified(ax) -> None:
    ax.set_xlim(0, 19)
    ax.set_ylim(0, 12)
    ax.set_aspect("equal")
    ax.axis("off")
    ax.set_title("Stage B (unified, always-streaming) - FSM + SRAM + init_done + carry")

    # FSM bubbles on left
    cx_idle, cy_idle = 2.3, 8.5
    cx_loop, cy_loop = 2.3, 4.5
    idle = Circle((cx_idle, cy_idle), 1.1, facecolor=C_FSM, edgecolor=C_EDGE, lw=1.5)
    loop = Circle((cx_loop, cy_loop), 1.1, facecolor=C_STAGE_B, edgecolor=C_EDGE, lw=1.5)
    ax.add_patch(idle)
    ax.add_patch(loop)
    _label(ax, cx_idle, cy_idle, "IDLE\nready=1", fs=10, color="white", weight="bold")
    _label(ax, cx_loop, cy_loop, "MERGE\n_LOOP\nready=0", fs=9, color="white", weight="bold")

    # IDLE -> LOOP
    _arrow(ax, cx_idle - 0.1, cy_idle - 1.1, cx_loop - 0.1, cy_loop + 1.1,
           color="#222", lw=2.0)
    _label(ax, cx_idle - 1.5, (cy_idle + cy_loop) / 2,
           "valid_in & ready\nchunk -> carry\nr <- 0",
           fs=8, color="#222", ha="right")

    # LOOP self-loop
    arc = FancyArrowPatch((cx_loop + 1.05, cy_loop - 0.3),
                          (cx_loop + 1.05, cy_loop + 0.3),
                          connectionstyle="arc3,rad=-1.2",
                          arrowstyle="->", mutation_scale=14,
                          linewidth=2.0, color="#222")
    ax.add_patch(arc)
    _label(ax, cx_loop + 2.5, cy_loop + 0.3,
           "merge(SRAM[r] | -inf, carry)\n"
           "wdata = top_P -> SRAM[r]\n"
           "init_done[r] <- 1\n"
           "carry <- bot_P\n"
           "r <- r + 1",
           fs=8, color="#222", ha="left")

    # LOOP -> IDLE
    _arrow(ax, cx_loop + 0.7, cy_loop + 1.0, cx_idle + 0.7, cy_idle - 1.0,
           color="#222", lw=2.0)
    _label(ax, cx_loop + 1.6, (cy_loop + cy_idle) / 2 - 0.2,
           "r == rows_used-1\nfinishing -> vseen",
           fs=8, color="#222", ha="left")

    # SRAM stack
    sram_x = 10.0
    n_rows_show = 4
    row_h = 0.7
    row_w = 4.5
    y_sram_bot = 1.2
    for r in range(n_rows_show):
        y = y_sram_bot + r * row_h
        _box(ax, sram_x, y, row_w, row_h * 0.85,
             f"SRAM row {n_rows_show - 1 - r}  (P lanes, desc)",
             fc=C_SRAM, fs=9, weight="bold")
    _label(ax, sram_x + row_w / 2, y_sram_bot + n_rows_show * row_h + 0.4,
           "Running SRAM (K_MAX/P rows x P*(32+idx_w))",
           fs=10, weight="bold", color=C_SRAM)

    # init_done bit-vec column
    init_x = sram_x - 1.5
    for r in range(n_rows_show):
        y = y_sram_bot + r * row_h
        _box(ax, init_x, y, 1.2, row_h * 0.85,
             f"id[{n_rows_show - 1 - r}]",
             fc=C_INIT, fs=9, weight="bold")
    _label(ax, init_x + 0.6, y_sram_bot + n_rows_show * row_h + 0.4,
           "init_done\n[K_MAX/P bits]",
           fs=9, weight="bold", color=C_INIT)

    # Merge unit
    merge_x = sram_x + row_w + 0.7
    _box(ax, merge_x, 3.7, 3.8, 1.4,
         "bitonic_merge_2p_full(P)\nlog2(2P) layers, all DESC",
         fc=C_STAGE_B, fs=9, weight="bold")

    # Carry reg
    _box(ax, merge_x + 0.8, 0.9, 2.0, 1.3,
         "carry reg\n(P lanes)",
         fc=C_CARRY, fs=9, weight="bold")

    # SRAM r-port -> read mux -> merge
    _arrow(ax, sram_x + row_w, 3.2, merge_x, 4.2,
           color=C_SRAM, lw=1.4, text="rdata", text_offset=(0.1, 0.1))
    _label(ax, sram_x + row_w + 0.35, 3.6,
           "read mux:\nid[r]? rdata\n: neg_inf_row(fmt_sel)",
           fs=8, color=C_INIT, ha="left")

    # init_done bit fed into mux (dynamic shift by raddr)
    _arrow(ax, init_x + 1.2, 2.8, merge_x - 0.05, 4.0,
           color=C_INIT, lw=1.2, style="->",
           text="id[raddr]", text_offset=(0.1, -0.2))

    # Merge -> SRAM (writeback)
    _arrow(ax, merge_x, 4.5, sram_x + row_w, 3.5,
           color=C_SRAM, lw=1.4, text="wdata=top_P", text_offset=(-2.6, 0.1))

    # Merge <-> carry
    _arrow(ax, merge_x + 1.8, 2.2, merge_x + 1.0, 3.7,
           color=C_CARRY, lw=1.5, text="carry.q", text_offset=(0.1, 0.1))
    _arrow(ax, merge_x + 1.5, 3.7, merge_x + 1.5, 2.2,
           color=C_CARRY, lw=1.5, text="carry<-bot_P", text_offset=(0.1, 0.3))

    # Chunk input (from Stage A) -> carry on IDLE->LOOP
    _box(ax, merge_x - 1.0, 10.2, 5.5, 0.9,
         "chunk_sorted_P  (Stage A out) - only on IDLE->LOOP edge",
         fc=C_CHUNK_IN, fs=9, weight="bold")
    _arrow(ax, merge_x + 1.7, 10.2, merge_x + 1.8, 2.3,
           color=C_CHUNK_IN, lw=1.5,
           text="chunk -> carry", text_offset=(0.15, 0.0))


# ═════════════════════════════════════════════════════════════════
# 5) K=4 numerical walkthrough (still illustrative; rows_used=1, fmt=fp32)
# ═════════════════════════════════════════════════════════════════

def draw_walkthrough(ax) -> None:
    ax.set_xlim(0, 17)
    ax.set_ylim(0, 11)
    ax.set_aspect("equal")
    ax.axis("off")
    ax.set_title("K=4 numerical walkthrough  (P=4, k_in=4, rows_used=1, fmt=fp32)")

    NEG = "−∞"

    rows = [
        ("chunk 0    in", ["3.0", "1.0", "4.0", "1.0"], C_CHUNK_IN),
        ("Stage A   out", ["4.0", "3.0", "1.0", "1.0"], C_STAGE_A),
        ("SRAM[0] @ read", [NEG, NEG, NEG, NEG], C_SRAM),
        ("init_done[0]  ", ["0", "—", "—", "—"], C_INIT),
        ("Stage B  merge", ["4.0", "3.0", "1.0", "1.0"], C_STAGE_B),
        ("SRAM[0].next  ", ["4.0", "3.0", "1.0", "1.0"], C_RUN_REG),

        ("chunk 1    in", ["5.0", "9.0", "2.0", "6.0"], C_CHUNK_IN),
        ("Stage A   out", ["9.0", "6.0", "5.0", "2.0"], C_STAGE_A),
        ("SRAM[0] @ read", ["4.0", "3.0", "1.0", "1.0"], C_SRAM),
        ("init_done[0]  ", ["1", "—", "—", "—"], C_INIT),
        ("Stage B  merge", ["9.0", "6.0", "5.0", "4.0"], C_STAGE_B),
        ("topk_out  ✓ ", ["9.0", "6.0", "5.0", "4.0"], C_OUTPUT),
    ]

    cell_w, cell_h = 1.3, 0.65
    base_x = 5.0
    base_y = 10.3
    for ri, (lbl, vals, fc) in enumerate(rows):
        y = base_y - ri * (cell_h + 0.13)
        _label(ax, base_x - 0.3, y + cell_h / 2, lbl, fs=10, ha="right",
               weight="bold", color="#222")
        for ci, v in enumerate(vals):
            _box(ax, base_x + ci * cell_w, y, cell_w * 0.95, cell_h, v,
                 fc=fc, fs=11, weight="bold")

    # Separator between the two chunks
    sep_y = base_y - 6 * (cell_h + 0.13) + cell_h * 1.3
    ax.plot([base_x - 0.5, base_x + 4 * cell_w + 0.3],
            [sep_y] * 2,
            color="#888", linestyle="--", lw=1.0)

    # Golden cross-check at right
    _box(ax, 12.2, 8.5, 4.4, 2.1,
         "Golden = sorted([3,1,4,1,5,9,2,6],\n"
         "                 desc)[:4]\n"
         "       = [9, 6, 5, 4]\n"
         "       ✓ matches topk_out",
         fc="#F0F8E6", ec=C_OUTPUT, fs=10, weight="bold")

    _label(ax, 14.4, 5.0,
           "Key observation:\n"
           "chunk 0 reads SRAM[0]\n"
           "with init_done[0]=0,\n"
           "so it gets a full row of\n"
           "−∞.  No startup sweep,\n"
           "no cfg_valid handshake.",
           fs=9, color="#333", ha="center")


# ═════════════════════════════════════════════════════════════════
# 6) Compose
# ═════════════════════════════════════════════════════════════════

def save_individual_panels() -> None:
    panels = [
        (draw_overview,         "topk_overview.png",        (16, 11)),
        (draw_stage_a_full,     "stage_a_full_p_sort.png",  (18, 11)),
        (draw_merge_2p_full,    "merge_2p_full_cell.png",   (15, 11)),
        (draw_stage_b_unified,  "stage_b_unified_fsm.png",  (19, 12)),
        (draw_walkthrough,      "walkthrough_k4.png",       (17, 10)),
    ]
    for fn, name, size in panels:
        fig, ax = plt.subplots(figsize=size, dpi=120)
        fn(ax)
        fig.tight_layout()
        out = HERE / name
        fig.savefig(out, dpi=120, bbox_inches="tight", facecolor="white")
        plt.close(fig)
        print(f"  wrote {out}  ({out.stat().st_size // 1024} KB)")


def save_all_in_one() -> None:
    fig = plt.figure(figsize=(22, 28), dpi=110)
    gs = fig.add_gridspec(3, 2, height_ratios=[1, 1.1, 1.05], hspace=0.18, wspace=0.10)

    ax1 = fig.add_subplot(gs[0, :])
    draw_overview(ax1)

    ax2 = fig.add_subplot(gs[1, 0])
    draw_stage_a_full(ax2)

    ax3 = fig.add_subplot(gs[1, 1])
    draw_merge_2p_full(ax3)

    ax4 = fig.add_subplot(gs[2, 0])
    draw_stage_b_unified(ax4)

    ax5 = fig.add_subplot(gs[2, 1])
    draw_walkthrough(ax5)

    fig.suptitle(
        "pyCircuit Top-K module (unified) - architecture overview",
        fontsize=18, weight="bold", y=0.995,
    )
    out = HERE / "topk_all.png"
    fig.savefig(out, dpi=110, bbox_inches="tight", facecolor="white")
    plt.close(fig)
    print(f"  wrote {out}  ({out.stat().st_size // 1024} KB)")


if __name__ == "__main__":
    print("Rendering Top-K (unified) architecture diagrams ...")
    save_individual_panels()
    save_all_in_one()
    print("Done.")
