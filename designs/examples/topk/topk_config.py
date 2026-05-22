"""Top-K module: compile-time configuration constants.

The unified Top-K module exposes ``fmt_sel`` and ``k_in`` as **runtime** inputs.
Only the hardware geometry is fixed at compile time:

  - ``P``     : chunk / bus width (also the sort network width)
  - ``K_MAX`` : maximum K supported (sets SRAM depth = K_MAX/P rows)
  - ``IDX_W`` : index width (bits)

bf16 / fp16 / fp32 / fp8_e4m3 / fp4_e2m1 are all selected at runtime via a 3-bit
``fmt_sel`` input. The hardware datapath is always ``VAL_W = 32`` bits wide;
narrow formats occupy the low bits of each 32-bit lane.

NaN/Inf semantics by format:

  - ``has_nan = True``  (bf16, fp16, fp32, fp8_e4m3): NaN is folded into
                        ``neg_inf_bits`` so it always loses a compare.
  - ``has_inf = True``  (bf16, fp16, fp32): real ``±inf`` encodings exist.
  - ``has_inf = False`` (fp8_e4m3, fp4_e2m1): no ``±inf``. The ``neg_inf_bits``
                        constant degenerates to NEG_MAX_FINITE — the most
                        negative finite value of the format. It still wins as
                        the "lose-anything" sentinel for SRAM init rows.
  - ``has_nan = False`` (fp4_e2m1): no NaN encoding at all. All 16 patterns
                        are valid finite values and the key transform is a
                        plain sign-magnitude → monotone unsigned.
"""
from __future__ import annotations

from dataclasses import dataclass


# ── Hardware geometry (compile-time) ──────────────────────────
P_DEFAULT: int = 256        # chunk width (elements per cycle = bus width)
K_MAX_DEFAULT: int = 4096   # K upper bound (SRAM depth = K_MAX/P rows)
IDX_W_DEFAULT: int = 13     # idx width (13 bits = up to 8192 elements; FP4 tile)


# ── Bus / tile geometry (top-level interface) ─────────────────
BUS_BYTES_PER_CYCLE: int = 512    # max payload bytes the upstream can deliver per cy
BUS_BITS_PER_CYCLE: int = BUS_BYTES_PER_CYCLE * 8        # 4096 bits
TILE_BYTES: int = 4096            # one PTO tile size (= one source operand of TTOPK)
CYCLES_PER_TILE: int = TILE_BYTES // BUS_BYTES_PER_CYCLE  # 8 cy at 512 B/cy


# ── Runtime fmt encoding (3-bit selector value) ───────────────
FMT_BF16: int = 0
FMT_FP16: int = 1
FMT_FP32: int = 2
FMT_FP8_E4M3: int = 3
FMT_FP4_E2M1: int = 4
# 5..7 reserved (e.g. fp8_e5m2, fp4_hifp4) — fmt_sel is 3 bits wide
FMT_SEL_W: int = 3


# ── Unified value width ───────────────────────────────────────
VAL_W: int = 32             # max of fp32; narrower fmts occupy the low bits


# ── Floating-point format metadata (compile-time lookup) ──────
@dataclass(frozen=True)
class FpFormat:
    """One row of the FP format table (compile-time metadata for code-gen).

    ``has_inf`` / ``has_nan`` describe whether the format has dedicated
    encodings for ±∞ and NaN. fp8_e4m3 has NaN but no ±∞; fp4_e2m1 has
    neither. The key transform consults these flags to pick the right
    NaN-fold and the right "lose-anything" sentinel (``neg_inf_bits``
    falls back to NEG_MAX_FINITE when ``has_inf=False``).
    """

    name: str
    width: int      # total bits in the *native* format
    exp_w: int      # exponent bits
    man_w: int      # mantissa bits
    bias: int       # exponent bias
    has_inf: bool = True
    has_nan: bool = True

    @property
    def sign_bit(self) -> int:
        return self.width - 1

    @property
    def exp_lsb(self) -> int:
        return self.man_w

    @property
    def exp_msb(self) -> int:
        return self.man_w + self.exp_w - 1

    @property
    def exp_all_ones(self) -> int:
        return (1 << self.exp_w) - 1

    @property
    def man_all_ones(self) -> int:
        return (1 << self.man_w) - 1

    @property
    def neg_max_finite_bits(self) -> int:
        """Most-negative finite bit pattern.

        - For IEEE-like formats with ±inf : sign=1, exp=all-1 - 1, man=all-1.
        - For fp8_e4m3 (no ±inf, has NaN) : sign=1, exp=all-1, man=all-1 - 1
                                            (since S.1111.111 is the lone NaN).
        - For fp4_e2m1 (no ±inf, no NaN)  : sign=1, exp=all-1, man=all-1.
        """
        if self.has_inf:
            # IEEE-like: ±inf is exp=all-1 & man=0; max finite has exp=all-1 - 1.
            max_finite_pos = ((self.exp_all_ones - 1) << self.man_w) | self.man_all_ones
            return (1 << self.sign_bit) | max_finite_pos
        if self.has_nan:
            # fp8_e4m3: lone NaN is S.1111.111. Max finite is exp=all-1, man=all-1-1.
            max_finite_pos = (self.exp_all_ones << self.man_w) | (self.man_all_ones - 1)
            return (1 << self.sign_bit) | max_finite_pos
        # fp4_e2m1: every encoding is finite. Max finite = exp=all-1, man=all-1.
        max_finite_pos = (self.exp_all_ones << self.man_w) | self.man_all_ones
        return (1 << self.sign_bit) | max_finite_pos

    @property
    def neg_inf_bits(self) -> int:
        """Bit pattern used as the "lose-anything" sentinel.

        Real ±inf when the format has it, else the most-negative finite value.
        Used:
          - As the NaN-fold target (so NaN never wins a compare).
          - As the SRAM init-row fill (untouched rows read as a row of -inf).
        """
        if self.has_inf:
            return (1 << self.sign_bit) | (self.exp_all_ones << self.man_w)
        return self.neg_max_finite_bits

    @property
    def max_finite_pos_bits(self) -> int:
        """Largest positive finite bit pattern of the format."""
        if self.has_inf:
            return ((self.exp_all_ones - 1) << self.man_w) | self.man_all_ones
        if self.has_nan:
            return (self.exp_all_ones << self.man_w) | (self.man_all_ones - 1)
        return (self.exp_all_ones << self.man_w) | self.man_all_ones


FP_FORMATS: dict[str, FpFormat] = {
    "bf16":     FpFormat(name="bf16",     width=16, exp_w=8, man_w=7,  bias=127, has_inf=True,  has_nan=True),
    "fp16":     FpFormat(name="fp16",     width=16, exp_w=5, man_w=10, bias=15,  has_inf=True,  has_nan=True),
    "fp32":     FpFormat(name="fp32",     width=32, exp_w=8, man_w=23, bias=127, has_inf=True,  has_nan=True),
    "fp8_e4m3": FpFormat(name="fp8_e4m3", width=8,  exp_w=4, man_w=3,  bias=7,   has_inf=False, has_nan=True),
    "fp4_e2m1": FpFormat(name="fp4_e2m1", width=4,  exp_w=2, man_w=1,  bias=1,   has_inf=False, has_nan=False),
}


# Index into FMTS_ORDERED is the runtime fmt_sel value (0..4 used, 5..7 reserved).
FMTS_ORDERED: tuple[FpFormat, ...] = (
    FP_FORMATS["bf16"],     # FMT_BF16     = 0
    FP_FORMATS["fp16"],     # FMT_FP16     = 1
    FP_FORMATS["fp32"],     # FMT_FP32     = 2
    FP_FORMATS["fp8_e4m3"], # FMT_FP8_E4M3 = 3
    FP_FORMATS["fp4_e2m1"], # FMT_FP4_E2M1 = 4
)
NUM_FMTS: int = len(FMTS_ORDERED)        # 5 active fmts; sel ∈ [0, NUM_FMTS)


def fmt_of(name: str) -> FpFormat:
    """Look up an FP format by short name (used by Python tests / model)."""
    if name not in FP_FORMATS:
        raise ValueError(f"unsupported fp format {name!r}; must be one of {list(FP_FORMATS)}")
    return FP_FORMATS[name]


def fmt_of_sel(sel: int) -> FpFormat:
    """Look up an FP format by 3-bit selector value."""
    if not (0 <= sel < NUM_FMTS):
        raise ValueError(
            f"fmt_sel out of range: {sel} (must be 0..{NUM_FMTS - 1}; 5..7 reserved)"
        )
    return FMTS_ORDERED[sel]


def sel_of_fmt(name: str) -> int:
    """Inverse of fmt_of_sel: map name → fmt_sel value."""
    fmt = fmt_of(name)
    for i, candidate in enumerate(FMTS_ORDERED):
        if candidate is fmt:
            return i
    raise ValueError(f"fmt {name!r} not in FMTS_ORDERED")


# ── Bit-width helpers (compile-time derivations) ──────────────
def k_in_w(K_MAX: int) -> int:
    """Bits needed to hold k_in ∈ [1, K_MAX]."""
    return max(1, K_MAX.bit_length())


def n_rows_max_of(P: int, K_MAX: int) -> int:
    return K_MAX // P


def rows_used_w(P: int, K_MAX: int) -> int:
    """Bits to hold rows_used ∈ [1, K_MAX/P]."""
    nrows = K_MAX // P
    return max(1, nrows.bit_length())


def drain_addr_w(P: int, K_MAX: int) -> int:
    """Bits needed to address one of K_MAX/P SRAM rows."""
    nrows = K_MAX // P
    if nrows <= 1:
        return 1
    return max(1, (nrows - 1).bit_length())


def lane_w(idx_w: int) -> int:
    """One (val, idx) lane in bits = VAL_W + idx_w."""
    return VAL_W + idx_w


def chunk_val_bus_w(P: int) -> int:
    """Packed P-element chunk_vals bus width: P * VAL_W."""
    return P * VAL_W


def chunk_idx_bus_w(P: int, idx_w: int) -> int:
    """Packed P-element chunk_idxs bus width: P * idx_w."""
    return P * idx_w


def lanes_per_cycle(fmt: FpFormat) -> int:
    """Lanes carried per BUS_BYTES_PER_CYCLE bus cycle for a given fmt."""
    return BUS_BITS_PER_CYCLE // fmt.width


def n_max_per_tile(fmt: FpFormat) -> int:
    """Maximum element count per 4 KB tile for a given fmt."""
    return TILE_BYTES // ((fmt.width + 7) // 8) if fmt.width >= 8 else (TILE_BYTES * 8) // fmt.width


def validate_params(*, P: int, K_MAX: int, idx_w: int = IDX_W_DEFAULT) -> None:
    """Validate top-level compile-time parameters."""
    if P <= 0 or (P & (P - 1)) != 0:
        raise ValueError(f"P must be a positive power of 2 (got {P})")
    if K_MAX < P or (K_MAX & (K_MAX - 1)) != 0:
        raise ValueError(f"K_MAX must be a power of 2 and >= P (got K_MAX={K_MAX}, P={P})")
    if K_MAX % P != 0:
        raise ValueError(f"K_MAX must be a multiple of P (got K_MAX={K_MAX}, P={P})")
    if idx_w <= 0:
        raise ValueError(f"idx_w must be positive (got {idx_w})")


# ── Testbench compile-time parameters ──────────────────────────────
# Small DUT footprint so the cycle-accurate sim is cheap.
DEFAULT_PARAMS: dict = {
    "P":     128,
    "K_MAX": 4096,
    "idx_w": 16,
}

TB_PRESETS: dict = {
    # smoke: 2 chunks × 12 cy + raddr settle + slack
    "smoke":   {"timeout": 256,  "finish": 64},
    "nightly": {"timeout": 1024, "finish": 256},
}

SIM_TIER: str = "normal"


# ── Large RTL workload ─────────────────────────────────────────
# Production-size workload (P=256, K_MAX=4096, idx_w=13). Index width is 13
# bits so it can hold the fp4 worst-case N=8192 elements per tile. The unified
# engine schedule for a single chunk is:
#     SORT 36 cy + MERGE_HALF 18·rows_used cy
# at P=256. For K=64 (rows_used=1), cy/chunk = 36 + 18 = 54.
LARGE_PARAMS: dict = {
    "P":     256,
    "K_MAX": 4096,
    "idx_w": 13,
}

LARGE_TB: dict = {
    "K":              64,
    "n_chunks":       16,        # default workload chunk count (16 * 256 = 4096 elements)
    "seed":           0xC0FFEE,
    # The large tb derives sa_latency / merge cycles from P / K at runtime.
    "sample_extra":   80,        # cy after engine becomes idle before sampling
    "row1_offset":    2,         # cycles between row 0 sample and row 1 sample (needs raddr settle)
    "finish_extra":   120,       # finish = last_sample + this
    "timeout_extra":  256,       # timeout = finish + this
}
