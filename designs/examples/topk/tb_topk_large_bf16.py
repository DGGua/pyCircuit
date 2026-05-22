"""Heavy Top-K RTL testbench — bf16 session.

Production geometry: P=256, K_MAX=4096, idx_w=13. See ``tb_topk_large_lib.py``
for the cycle plan / golden-model details.
"""
from __future__ import annotations

import sys
from pathlib import Path

from pycircuit import Tb, compile_cycle_aware, testbench

_THIS_DIR = Path(__file__).resolve().parent
if str(_THIS_DIR) not in sys.path:
    sys.path.insert(0, str(_THIS_DIR))

from topk import build as _topk_build  # noqa: E402
from topk_config import LARGE_PARAMS  # noqa: E402
from tb_topk_large_lib import drive_large_tb, override_build_defaults  # noqa: E402


_FMT = "bf16"
_NAME = "tb_topk_large_bf16"

build = override_build_defaults(_topk_build, LARGE_PARAMS)


@testbench
def tb(t: Tb) -> None:
    drive_large_tb(t, fmt_name=_FMT)


if __name__ == "__main__":
    print(compile_cycle_aware(build, name=_NAME, **LARGE_PARAMS).emit_mlir())
