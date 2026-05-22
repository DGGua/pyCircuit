"""Top-K Histogram compile-time configuration and test presets.

Mirrors the parameter table in the plan §3 and the contract in arch.md §3.1.
Only :data:`DEFAULT_PARAMS` is consumed by the RTL `build()`; everything else
is testbench-side.
"""
from __future__ import annotations

from typing import Any, Dict


# ─── Hardware geometry (compile-time) ────────────────────────────
DEFAULT_PARAMS: Dict[str, int] = {
    "N":             1024,    # element count per Top-K invocation
    "LANE_NUM":      128,     # lanes per beat / bus width = LANE_NUM * 32
    "BURST_LEN":     8,       # input / output beats per invocation (= N / LANE_NUM)
    "K_MAX":         1024,    # maximum supported K
    "K_MAX_BITS":    11,      # ceil(log2(K_MAX + 1))
    "RADIX_BITS":    8,       # one byte per radix round → 4 rounds
    "HIST_W":        11,      # ceil(log2(N + 1))
    "VAL_W":         32,      # fp32
    "ELEM_IDX_W":    10,      # ceil(log2(N))
}


# ─── Testbench presets ───────────────────────────────────────────
TB_PRESETS: Dict[str, Dict[str, int]] = {
    "smoke":   {"timeout": 256,  "finish": 90,  "K": 4,   "seed": 0},
    "normal":  {"timeout": 256,  "finish": 90,  "K": 128, "seed": 1},
    "nightly": {"timeout": 1024, "finish": 256, "K": 900, "seed": 42},
}

SIM_TIER: str = "normal"


# ─── Parameter validation ────────────────────────────────────────
def validate_params(params: Dict[str, Any] | None = None) -> None:
    """Validate a parameter dict (defaults to :data:`DEFAULT_PARAMS`)."""
    p = dict(DEFAULT_PARAMS) if params is None else dict(params)

    N = p["N"]
    LANE = p["LANE_NUM"]
    BURST = p["BURST_LEN"]
    K_MAX = p["K_MAX"]
    K_MAX_BITS = p["K_MAX_BITS"]
    RADIX_BITS = p["RADIX_BITS"]
    HIST_W = p["HIST_W"]
    VAL_W = p["VAL_W"]
    ELEM_IDX_W = p["ELEM_IDX_W"]

    if N <= 0 or (N & (N - 1)) != 0:
        raise ValueError(f"N must be a positive power of 2 (got {N})")
    if LANE <= 0 or (LANE & (LANE - 1)) != 0:
        raise ValueError(f"LANE_NUM must be a positive power of 2 (got {LANE})")
    if BURST * LANE != N:
        raise ValueError(
            f"BURST_LEN * LANE_NUM must equal N "
            f"(got BURST_LEN={BURST}, LANE_NUM={LANE}, N={N})"
        )
    if K_MAX <= 0 or K_MAX > N:
        raise ValueError(f"K_MAX must be in (0, N] (got K_MAX={K_MAX}, N={N})")
    if K_MAX_BITS < (K_MAX + 1).bit_length() - 1:
        # We want K_MAX_BITS to hold values 0..K_MAX inclusive.
        need = max(1, (K_MAX).bit_length())
        if K_MAX_BITS < need:
            raise ValueError(
                f"K_MAX_BITS={K_MAX_BITS} too small for K_MAX={K_MAX} (need {need})"
            )
    if RADIX_BITS != 8:
        raise ValueError(f"v1 only supports RADIX_BITS=8 (got {RADIX_BITS})")
    if VAL_W != 32:
        raise ValueError(f"v1 only supports VAL_W=32 / fp32 (got {VAL_W})")
    if 32 % RADIX_BITS != 0:
        raise ValueError(
            f"VAL_W ({VAL_W}) must be a multiple of RADIX_BITS ({RADIX_BITS})"
        )
    if HIST_W < (N).bit_length():
        raise ValueError(
            f"HIST_W={HIST_W} too small to count up to N={N} (need {(N).bit_length()})"
        )
    if ELEM_IDX_W < (N - 1).bit_length():
        raise ValueError(
            f"ELEM_IDX_W={ELEM_IDX_W} too small for N={N} "
            f"(need {(N - 1).bit_length()})"
        )


# Run validation on import so misconfiguration is caught early.
validate_params()


if __name__ == "__main__":
    print("DEFAULT_PARAMS:")
    for k, v in DEFAULT_PARAMS.items():
        print(f"  {k:12} = {v}")
    print(f"\nTB_PRESETS: {list(TB_PRESETS)}")
    print(f"SIM_TIER:   {SIM_TIER}")
    print("\ntopk_histogram_config.py: validate_params() OK")
