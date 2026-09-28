#!/usr/bin/env python3
from __future__ import annotations

import importlib.util
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

TOOLS = Path(__file__).resolve().parent
sys.path.insert(0, str(TOOLS))

from cpp_pch_headers import select_device_hpp_headers


class TestCppPchHeaders(unittest.TestCase):
    def test_selects_module_primary_hpp(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            mod = root / "topk_histogram"
            mod.mkdir()
            hpp = mod / "topk_histogram.hpp"
            hpp.write_text("#pragma once\n", encoding="utf-8")
            (mod / "helper.hpp").write_text("#pragma once\n", encoding="utf-8")
            got = select_device_hpp_headers([str(hpp), str(mod / "helper.hpp")])
            self.assertEqual(got, [str(hpp.resolve())])

    def test_multiple_modules(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            paths: list[str] = []
            for name in ("foo", "bar"):
                mod = root / name
                mod.mkdir()
                hpp = mod / f"{name}.hpp"
                hpp.write_text("#pragma once\n", encoding="utf-8")
                paths.append(str(hpp))
            got = select_device_hpp_headers(paths)
            self.assertEqual(got, sorted(str(Path(p).resolve()) for p in paths))

    def test_empty(self) -> None:
        self.assertEqual(select_device_hpp_headers([]), [])

    def test_duplicate_and_non_header_inputs(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            header = root / "dut/dut.hpp"
            got = select_device_hpp_headers([str(header), str(header), "", "dut.cpp"])
            self.assertEqual(got, [str(header.resolve())])

    def test_wheel_stages_pch_helper(self) -> None:
        root = TOOLS.parents[1]
        path = root / "packaging/wheel/create_wheel.py"
        spec = importlib.util.spec_from_file_location("pyc_pch_wheel", path)
        wheel = importlib.util.module_from_spec(spec)
        assert spec.loader is not None
        with mock.patch.object(sys, "path", [str(path.parent), *sys.path]):
            spec.loader.exec_module(wheel)
        with tempfile.TemporaryDirectory() as tmp:
            stage_root = Path(tmp)
            install = stage_root / "install"
            install.mkdir()
            def inspect_stage(command, **kwargs):
                staged = Path(kwargs["cwd"]) / "pycircuit/_tools/cpp_pch_headers.py"
                self.assertEqual(staged.read_bytes(), (TOOLS / "cpp_pch_headers.py").read_bytes())
            with mock.patch.object(wheel.subprocess, "run", side_effect=inspect_stage) as run:
                self.assertEqual(wheel.main(["--install-dir", str(install),
                                             "--out-dir", str(stage_root / "dist"),
                                             "--build-root", str(stage_root / "build")]), 0)
                run.assert_called_once()


if __name__ == "__main__":
    unittest.main()
