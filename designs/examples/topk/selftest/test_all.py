#!/usr/bin/env python3
"""Run all topk pure-Python self-tests."""
from __future__ import annotations

import sys
from pathlib import Path

_TOPK = Path(__file__).resolve().parent.parent
if str(_TOPK) not in sys.path:
    sys.path.insert(0, str(_TOPK))

from selftest.test_bitonic_schedule import run as run_bitonic_schedule
from selftest.test_fp_compare import run as run_fp_compare

_TESTS = [
    ("fp_compare", run_fp_compare),
    ("bitonic_schedule", run_bitonic_schedule),
]


def main() -> None:
    print("Running all topk self-tests...")
    for name, run in _TESTS:
        print(f"\n=== {name} ===")
        run()
    print(f"\nAll {len(_TESTS)} self-test modules passed.")


if __name__ == "__main__":
    main()
