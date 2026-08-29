from __future__ import annotations

import argparse
import json
import unittest
from pathlib import Path

from pycircuit.sim_benchmark import (
    OBS_PREFIX,
    RESULT_PREFIX,
    BenchmarkError,
    NativeResult,
    add_benchmark_arguments,
    compare_observations,
    load_interface,
    parse_native_output,
    render_common_harness_header,
    render_cpp_harness,
    render_verilator_harness,
    run_benchmark,
    summarize_samples,
)


def _manifest(
    *,
    top: str = "top",
    inputs: tuple[tuple[str, str], ...],
    outputs: tuple[tuple[str, str], ...],
) -> dict[str, object]:
    return {
        "top": top,
        "modules": [
            {
                "name": top,
                "arg_names": [name for name, _ in inputs],
                "arg_types": [type_text for _, type_text in inputs],
                "result_names": [name for name, _ in outputs],
                "result_types": [type_text for _, type_text in outputs],
            }
        ],
    }


def _native_result(*, seconds: float, throughput: float) -> NativeResult:
    return NativeResult(
        backend="cpp",
        mode="comb",
        iterations=100,
        seconds=seconds,
        throughput_per_second=throughput,
        verify_digest="0xverify",
        digest="0xdigest",
        final_digest="0xfinal",
        external_wall_seconds=seconds + 0.01,
    )


class TestBenchmarkInterface(unittest.TestCase):
    def test_load_interface_comb_sanitizes_and_uniquifies_names(self) -> None:
        interface = load_interface(
            _manifest(
                top="comb-top",
                inputs=(("in-a", "i65"), ("in_a", "i1"), ("3rd", "i8")),
                outputs=(("in-a", "i128"), ("out value", "i9")),
            )
        )

        self.assertEqual(interface.mode, "comb")
        self.assertEqual(interface.operation, "evaluations")
        self.assertEqual(interface.observation, "settled-comb")
        self.assertEqual(interface.emitted_top, "comb_top")
        self.assertEqual(
            [port.emitted_name for port in interface.inputs],
            ["in_a", "in_a_2", "_3rd"],
        )
        self.assertEqual(
            [port.emitted_name for port in interface.outputs],
            ["in_a_3", "out_value"],
        )
        self.assertEqual([port.role for port in interface.inputs], ["data", "data", "data"])

    def test_load_interface_auto_selects_clocked_mode(self) -> None:
        interface = load_interface(
            _manifest(
                top="counter",
                inputs=(("clk", "!pyc.clock"), ("rst", "!pyc.reset"), ("d", "i8")),
                outputs=(("q", "i8"),),
            )
        )

        self.assertEqual(interface.mode, "clocked")
        self.assertEqual(interface.operation, "cycles")
        self.assertEqual(interface.observation, "xfer")
        self.assertEqual([port.raw_name for port in interface.clocks], ["clk"])
        self.assertEqual([port.raw_name for port in interface.resets], ["rst"])
        self.assertEqual([port.raw_name for port in interface.data_inputs], ["d"])

    def test_load_interface_rejects_multiple_clocks_and_unsupported_types(self) -> None:
        multiple_clocks = _manifest(
            inputs=(("clk_a", "!pyc.clock"), ("clk_b", "!pyc.clock")),
            outputs=(("out", "i1"),),
        )
        with self.assertRaisesRegex(BenchmarkError, r"multiple clocks \(clk_a, clk_b\)"):
            load_interface(multiple_clocks)

        unsupported_type = _manifest(
            inputs=(("bundle", "!pyc.bundle"),),
            outputs=(("out", "i1"),),
        )
        with self.assertRaisesRegex(BenchmarkError, r"port 'bundle' has unsupported type '!pyc.bundle'"):
            load_interface(unsupported_type)


class TestBenchmarkHarnessRendering(unittest.TestCase):
    def test_wide_i65_and_i128_ports_use_correct_cpp_and_verilator_words(self) -> None:
        interface = load_interface(
            _manifest(
                top="wide-top",
                inputs=(("input-65", "i65"), ("input-128", "i128")),
                outputs=(("output-65", "i65"), ("output-128", "i128")),
            )
        )
        common = render_common_harness_header(interface, input_table_size=8)
        cpp = render_cpp_harness(
            interface,
            top_header="wide_top.hpp",
            reset_cycles=2,
            reset_settle_cycles=1,
        )
        verilator = render_verilator_harness(
            interface,
            reset_cycles=2,
            reset_settle_cycles=1,
        )

        self.assertEqual(interface.inputs[0].words32, 3)
        self.assertEqual(interface.inputs[0].words64, 2)
        self.assertEqual(interface.inputs[1].words32, 4)
        self.assertEqual(interface.inputs[1].words64, 2)
        self.assertIn("std::array<std::uint32_t, 3> input_65{};", common)
        self.assertIn("std::array<std::uint32_t, 4> input_128{};", common)
        self.assertIn("frame.input_65[2] &= 0x1u;", common)
        self.assertIn("std::unique_ptr<InputTable> makeInputs", common)
        self.assertIn("errno != ERANGE", common)

        self.assertIn("pyc::cpp::Wire<65>({", cpp)
        self.assertIn("pyc::cpp::Wire<128>({", cpp)
        self.assertIn("frame.input_65[2]", cpp)
        self.assertIn("0x1ull", cpp)
        self.assertIn("dut.output_65.word(1)", cpp)
        self.assertIn("dut.output_128.word(1)", cpp)

        # Verilator exposes wide ports as VlWide-like objects indexed through
        # .at(word), using 32-bit words rather than pyCircuit's 64-bit words.
        self.assertIn("dut.input_65.at(0) = frame.input_65[0];", verilator)
        self.assertIn("dut.input_65.at(2) = (frame.input_65[2] & 0x1u);", verilator)
        self.assertIn("dut.input_128.at(3) = frame.input_128[3];", verilator)
        self.assertIn("dut.output_65.at(2)", verilator)
        self.assertIn("dut.output_128.at(3)", verilator)
        self.assertIn("0x1ull", verilator)


class TestNativeResultProcessing(unittest.TestCase):
    def test_parse_native_output_extracts_result_and_observations(self) -> None:
        payload = {
            "schema_version": 1,
            "backend": "cpp",
            "mode": "clocked",
            "iterations": 123,
            "seconds": 0.25,
            "throughput_per_second": 492.0,
            "verify_digest": "0x11",
            "digest": "0x22",
            "final_digest": "0x33",
        }
        observation = "PYC_BENCH_OBS cycle=7 port=q width=65 word=1 value=0x0000000000000001"
        output = "\n".join(
            [
                "ordinary diagnostic",
                observation,
                RESULT_PREFIX + json.dumps(payload),
                "trailing diagnostic",
            ]
        )

        result, observations = parse_native_output(output, external_wall_seconds=0.31)

        self.assertEqual(result.backend, "cpp")
        self.assertEqual(result.mode, "clocked")
        self.assertEqual(result.iterations, 123)
        self.assertEqual(result.seconds, 0.25)
        self.assertEqual(result.throughput_per_second, 492.0)
        self.assertEqual(result.external_wall_seconds, 0.31)
        self.assertEqual(observations, [observation])

    def test_compare_observations_reports_the_first_difference(self) -> None:
        common = OBS_PREFIX + "cycle=0 port=q width=1 word=0 value=0x0000000000000000"
        cpp_first_difference = OBS_PREFIX + "cycle=1 port=q width=1 word=0 value=0x0000000000000001"
        verilator_first_difference = OBS_PREFIX + "cycle=1 port=q width=1 word=0 value=0x0000000000000002"

        with self.assertRaisesRegex(BenchmarkError, r"observation 1") as caught:
            compare_observations(
                [common, cpp_first_difference, "later C++ difference"],
                [common, verilator_first_difference, "later Verilator difference"],
            )

        message = str(caught.exception)
        self.assertIn(cpp_first_difference, message)
        self.assertIn(verilator_first_difference, message)
        self.assertNotIn("later C++ difference", message)

    def test_summarize_samples_reports_median_percentiles_and_mad(self) -> None:
        stats = summarize_samples(
            [
                _native_result(seconds=4.0, throughput=10.0),
                _native_result(seconds=2.0, throughput=20.0),
                _native_result(seconds=3.0, throughput=30.0),
                _native_result(seconds=1.0, throughput=40.0),
            ]
        )

        self.assertEqual(stats["count"], 4)
        self.assertEqual(stats["median_seconds"], 2.5)
        self.assertEqual(stats["min_seconds"], 1.0)
        self.assertEqual(stats["max_seconds"], 4.0)
        self.assertEqual(stats["median_throughput_per_second"], 25.0)
        self.assertEqual(stats["p25_throughput_per_second"], 17.5)
        self.assertEqual(stats["p75_throughput_per_second"], 32.5)
        self.assertEqual(stats["mad_throughput_per_second"], 10.0)


class TestBenchmarkArgumentGuards(unittest.TestCase):
    @staticmethod
    def _args(*extra: str) -> argparse.Namespace:
        parser = argparse.ArgumentParser()
        add_benchmark_arguments(parser)
        return parser.parse_args([str(Path(__file__).resolve()), *extra])

    def test_rejects_seed_outside_uint64_before_build(self) -> None:
        for seed in ("-1", str(1 << 64)):
            with self.subTest(seed=seed):
                with self.assertRaisesRegex(BenchmarkError, r"unsigned 64-bit"):
                    run_benchmark(self._args("--seed", seed))

    def test_rejects_whitespace_in_out_dir_before_build(self) -> None:
        with self.assertRaisesRegex(BenchmarkError, r"must not contain whitespace"):
            run_benchmark(self._args("--out-dir", "/tmp/pyc benchmark unit"))


if __name__ == "__main__":
    unittest.main()
