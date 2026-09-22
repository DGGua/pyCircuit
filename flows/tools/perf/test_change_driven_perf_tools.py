#!/usr/bin/env python3
"""Self-tests for change-driven statistics and A/B performance tools."""

from __future__ import annotations

import json
import tempfile
import unittest
from pathlib import Path
from unittest import mock

import analyze_change_driven_stats as analyzer
import run_change_driven_ab as ab_runner

HERE = Path(__file__).resolve().parent
FIXTURES = HERE / "testdata"


class AnalyzerTest(unittest.TestCase):
    def test_single_file_summary(self) -> None:
        summary = analyzer.summarize_file(FIXTURES / "change_driven_stats_run1.jsonl")
        self.assertEqual(summary["top_path"], "top")
        self.assertEqual(len(summary["records"]), 2)
        self.assertAlmostEqual(summary["top"]["source_change_rate"], 0.2)
        self.assertAlmostEqual(summary["top"]["coalesced_rate"], 0.2)
        self.assertAlmostEqual(summary["top"]["comb_calls_per_stage_eval"], 1.5)
        self.assertAlmostEqual(summary["top"]["semantic_change_rate"], 0.4)

    def test_repeated_run_median_min_max(self) -> None:
        summaries = [
            analyzer.summarize_file(FIXTURES / f"change_driven_stats_run{run}.jsonl")
            for run in (1, 2, 3)
        ]
        combined = analyzer.aggregate(summaries)
        source_checks = combined["metrics"]["source_checks"]
        self.assertEqual(source_checks, {"median": 100, "min": 80, "max": 120})
        self.assertIn(
            "| `source_checks` | 100 | 80 | 120 |",
            analyzer.render_markdown(summaries, combined),
        )

    def test_strict_schema_rejects_unknown_and_invalid_fields(self) -> None:
        base = json.loads(
            (FIXTURES / "change_driven_stats_run1.jsonl")
            .read_text(encoding="utf-8")
            .splitlines()[0]
        )
        cases = []
        unknown = dict(base, unexpected=1)
        cases.append(unknown)
        invalid = dict(base, source_changes=base["source_checks"] + 1)
        cases.append(invalid)
        boolean = dict(base, max_ready=True)
        cases.append(boolean)
        with tempfile.TemporaryDirectory() as temp:
            path = Path(temp) / "invalid.jsonl"
            for row in cases:
                path.write_text(json.dumps(row) + "\n", encoding="utf-8")
                with self.assertRaises(analyzer.StatsError):
                    analyzer.load_rows(path)


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
