#!/usr/bin/env python3
from __future__ import annotations

import sys
from pathlib import Path


def _load_in_tree_frontend() -> None:
    root = Path(__file__).resolve().parents[3]
    frontend = root / "compiler" / "frontend"
    value = str(frontend)
    if value not in sys.path:
        sys.path.insert(0, value)


def main() -> int:
    _load_in_tree_frontend()
    from pycircuit.sim_benchmark import standalone_main

    return standalone_main()


if __name__ == "__main__":
    raise SystemExit(main())
