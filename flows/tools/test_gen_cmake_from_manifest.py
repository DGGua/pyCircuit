#!/usr/bin/env python3
from __future__ import annotations

import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

TOOLS = Path(__file__).resolve().parent
GEN = TOOLS / "gen_cmake_from_manifest.py"


class TestGenCmakeFromManifest(unittest.TestCase):
    def _run(self, manifest: dict, out_dir: Path) -> str:
        out_dir.mkdir(parents=True, exist_ok=True)
        manifest_path = out_dir / "manifest.json"
        manifest_path.write_text(json.dumps(manifest), encoding="utf-8")
        subprocess.run(
            [sys.executable, str(GEN), "--manifest", str(manifest_path), "--out-dir", str(out_dir)],
            check=True,
            capture_output=True,
            text=True,
        )
        return (out_dir / "CMakeLists.txt").read_text(encoding="utf-8")

    def test_pch_block_present(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            mod = root / "device" / "dut"
            mod.mkdir(parents=True)
            hpp = mod / "dut.hpp"
            cpp = mod / "dut__core.cpp"
            tb = root / "tb.cpp"
            hpp.write_text("#pragma once\n", encoding="utf-8")
            cpp.write_text("#include \"dut.hpp\"\n", encoding="utf-8")
            tb.write_text("#include \"dut.hpp\"\n", encoding="utf-8")
            text = self._run(
                {
                    "sources": [str(cpp)],
                    "tb_cpp": str(tb),
                    "include_dirs": [str(mod.parent), str(mod)],
                    "headers": [str(hpp)],
                    "precompile_headers": [str(hpp.resolve())],
                    "cxx_standard": "c++17",
                },
                root / "cmake_src",
            )
            self.assertIn("target_precompile_headers(pyc_tb PRIVATE", text)
            self.assertIn("dut.hpp", text)

    def test_pch_block_absent_when_empty(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            mod = root / "device" / "dut"
            mod.mkdir(parents=True)
            hpp = mod / "dut.hpp"
            cpp = mod / "dut__core.cpp"
            tb = root / "tb.cpp"
            hpp.write_text("#pragma once\n", encoding="utf-8")
            cpp.write_text("#include \"dut.hpp\"\n", encoding="utf-8")
            tb.write_text("#include \"dut.hpp\"\n", encoding="utf-8")
            text = self._run(
                {
                    "sources": [str(cpp)],
                    "tb_cpp": str(tb),
                    "include_dirs": [str(mod.parent), str(mod)],
                    "headers": [str(hpp)],
                    "cxx_standard": "c++17",
                },
                root / "cmake_src",
            )
            self.assertNotIn("target_precompile_headers", text)


if __name__ == "__main__":
    unittest.main()
