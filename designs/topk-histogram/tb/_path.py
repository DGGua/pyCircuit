"""Common sys.path shim: import this once at the top of each tb file so the
parent directory (`designs/topk-histogram/`) is on the path and imports like
``from topk_histogram import build`` resolve.

Usage:
    import _path  # noqa: F401
    from topk_histogram import build
"""
from __future__ import annotations

import sys
from pathlib import Path

_ROOT = Path(__file__).resolve().parent.parent
if str(_ROOT) not in sys.path:
    sys.path.insert(0, str(_ROOT))
