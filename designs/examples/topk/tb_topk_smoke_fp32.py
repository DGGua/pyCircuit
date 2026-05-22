"""Smoke testbench — fp32 path of the unified Top-K engine."""
from __future__ import annotations

import sys
from pathlib import Path

from pycircuit import Tb, compile_cycle_aware, testbench

_THIS_DIR = Path(__file__).resolve().parent
if str(_THIS_DIR) not in sys.path:
    sys.path.insert(0, str(_THIS_DIR))

from topk import build  # noqa: E402
from topk_config import DEFAULT_PARAMS  # noqa: E402
from tb_topk_smoke_lib import drive_smoke_tb  # noqa: E402


@testbench
def tb(t: Tb) -> None:
    drive_smoke_tb(t, fmt_name="fp32")


if __name__ == "__main__":
    print(compile_cycle_aware(build, name="tb_topk_smoke_fp32",
                              **DEFAULT_PARAMS).emit_mlir())
