#!/usr/bin/env python3
"""Self-tests for the change-driven wall-clock A/B performance tool."""

from __future__ import annotations

import tempfile
import unittest
from pathlib import Path
from unittest import mock

import run_change_driven_ab as ab_runner

class AbRunnerTest(unittest.TestCase):
    def test_default_one_warmup_seven_interleaved_runs(self) -> None:
        commands = dict.fromkeys(ab_runner.VARIANTS, ":")
        with tempfile.TemporaryDirectory() as temp:
            output = Path(temp)
            with mock.patch.object(
                ab_runner, "_run_once", return_value=1.0
            ) as run_once:
                result = ab_runner.run_benchmark(commands, output_dir=output)
            self.assertEqual(result["warmups"], 1)
            self.assertEqual(result["repeats"], 7)
            self.assertEqual(run_once.call_count, 24)
            for variant in ab_runner.VARIANTS:
                samples = result["variants"][variant]["samples_seconds"]
                self.assertEqual(len(samples), 7)
                self.assertEqual(samples, [1.0] * 7)

    def test_nearest_rank_p95(self) -> None:
        self.assertEqual(ab_runner._percentile([1, 2, 3, 4, 5, 6, 7], 0.95), 7)


if __name__ == "__main__":
    unittest.main()
