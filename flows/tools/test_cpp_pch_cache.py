#!/usr/bin/env python3
from __future__ import annotations

import hashlib
import json
import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
FRONTEND = ROOT / "compiler" / "frontend"
sys.path.insert(0, str(FRONTEND))

from pycircuit.cli import _backend_build_flag_hashes, _gather_cpp_headers, _gather_cpp_sources


class TestCppManifestArtifacts(unittest.TestCase):
    def test_manifest_excludes_stale_shards_and_headers(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            mod = root / "dut"
            mod.mkdir()
            for name in ("dut__core_000.cpp", "dut__core.cpp", "dut__core_999.cpp", "dut.hpp", "obsolete.hpp"):
                (mod / name).write_text("// fixture\n")
            (mod / "cpp_compile_manifest.json").write_text(json.dumps({
                "sources": [{"path": "dut__core_000.cpp"}, {"path": str(mod / "dut__core_000.cpp")}],
                "top_header": "dut.hpp",
            }))
            self.assertEqual(_gather_cpp_sources(root), [mod / "dut__core_000.cpp"])
            self.assertEqual(_gather_cpp_headers(root), [mod / "dut.hpp"])

    def test_active_modules_exclude_removed_module_outputs(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            for name in ("active", "removed"):
                mod = root / name
                mod.mkdir()
                (mod / f"{name}.cpp").write_text("// fixture\n")
                (mod / f"{name}.hpp").write_text("// fixture\n")
                (mod / "cpp_compile_manifest.json").write_text(json.dumps({
                    "sources": [{"path": f"{name}.cpp"}], "top_header": f"{name}.hpp"}))
            self.assertEqual(_gather_cpp_sources(root, module_names=["active"]), [root / "active/active.cpp"])
            self.assertEqual(_gather_cpp_headers(root, module_names=["active"]), [root / "active/active.hpp"])

    def test_legacy_fallback_and_required_manifest(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            mod = root / "dut"
            mod.mkdir()
            (mod / "dut.cpp").write_text("// fixture\n")
            (mod / "dut.hpp").write_text("// fixture\n")
            self.assertEqual(_gather_cpp_sources(root), [mod / "dut.cpp"])
            self.assertEqual(_gather_cpp_headers(root), [mod / "dut.hpp"])
            with self.assertRaisesRegex(SystemExit, r"missing C\+\+ compile manifest"):
                _gather_cpp_sources(root, module_names=["dut"])

    def test_malformed_manifest_does_not_fall_back_to_stale_sources(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root / "stale.cpp").write_text("// fixture\n")
            manifest = root / "cpp_compile_manifest.json"
            for content in ("{", "[]", '{}', '{"sources": {}}', '{"sources": [{}]}'):
                with self.subTest(content=content):
                    manifest.write_text(content)
                    with self.assertRaisesRegex(SystemExit, r"C\+\+ compile manifest"):
                        _gather_cpp_sources(root)

    def test_missing_manifest_source_and_header_are_errors(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root / "cpp_compile_manifest.json").write_text(json.dumps({
                "sources": [{"path": "missing.cpp"}], "top_header": "missing.hpp"}))
            with self.assertRaisesRegex(SystemExit, r"missing C\+\+ source"):
                _gather_cpp_sources(root)
            with self.assertRaisesRegex(SystemExit, r"missing C\+\+ header"):
                _gather_cpp_headers(root)


class TestCppPchCacheKeys(unittest.TestCase):
    def test_pch_only_invalidates_cpp_key(self) -> None:
        shared_flags = {
            "pycc": "/toolchain/bin/pycc",
            "logic_depth": 32,
            "target": "both",
        }

        shared_off, cpp_off = _backend_build_flag_hashes(shared_flags, cpp_pch=False)
        shared_on, cpp_on = _backend_build_flag_hashes(shared_flags, cpp_pch=True)

        self.assertEqual(shared_off, shared_on)
        self.assertNotEqual(cpp_off, cpp_on)
        self.assertNotIn("cpp_pch", shared_flags)

    @unittest.skipUnless(os.environ.get("PYC_PCH_CACHE_INTEGRATION") == "1",
                         "enabled by the device PCH smoke gate")
    def test_cli_toggle_preserves_verilog_and_jit_artifacts(self) -> None:
        with tempfile.TemporaryDirectory(prefix="pyc-pch-cache-") as tmp:
            out = Path(tmp)
            env = os.environ.copy()
            env["PYTHONPATH"] = str(FRONTEND)
            env["PYTHONDONTWRITEBYTECODE"] = "1"
            command = [sys.executable, "-m", "pycircuit.cli", "build",
                       str(ROOT / "designs/examples/counter/tb_counter.py"),
                       "--out-dir", str(out), "--target", "both", "--jobs", "2"]
            baseline = None
            previous_cpp_key = None
            stale_sources = []
            for index, enabled in enumerate((False, True, True, False)):
                result = subprocess.run(command + (["--cpp-pch"] if enabled else []),
                                        cwd=ROOT, env=env, text=True,
                                        stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
                self.assertEqual(result.returncode, 0, result.stdout[-12000:])
                cache = json.loads((out / ".build_cache.json").read_text())
                project = json.loads((out / "project_manifest.json").read_text())
                cpp_project = json.loads((out / "cpp_project_manifest.json").read_text())
                module_manifests = sorted((out / "device/cpp").rglob("cpp_compile_manifest.json"))
                self.assertTrue(module_manifests)
                # Aggregating PCH headers must never overwrite a module manifest
                # with project metadata (the two paths share one Python scope).
                for path in module_manifests:
                    module = json.loads(path.read_text())
                    self.assertTrue(module["sources"])
                    self.assertIsInstance(module["sources"][0], dict)
                    self.assertEqual(bool(module.get("precompile_headers")), enabled)
                self.assertEqual(bool(cpp_project.get("precompile_headers")), enabled)
                expected_sources = sorted({str((path.parent / source["path"]).resolve())
                                           for path in module_manifests
                                           for source in json.loads(path.read_text())["sources"]})
                self.assertEqual(cpp_project["sources"], expected_sources)
                self.assertEqual(cpp_project["tb_cpp"], str(out / "tb" / f'{project["testbench"]["name"]}.cpp'))
                self.assertTrue(Path(cpp_project["tb_cpp"]).is_file())
                if index == 0:
                    # Simulate pre-upgrade unsplit core and obsolete numbered
                    # shards in the same output directory. Neither is active
                    # in the compiler's current source manifest.
                    for path in module_manifests:
                        for name in ("obsolete__core.cpp", "obsolete__core_999.cpp"):
                            stale = path.parent / name
                            stale.write_text('#error stale shard must not be compiled\n')
                            stale_sources.append(stale)
                else:
                    self.assertTrue(all(path.is_file() for path in stale_sources))
                    self.assertTrue(all(str(path) not in cpp_project["sources"] for path in stale_sources))
                if enabled:
                    self.assertTrue(any(p.suffix in (".gch", ".pch")
                                        for p in (out / "cpp_build/build").rglob("*cmake_pch*")))
                files = [*out.rglob("*.pyc"), *(out / "device/verilog").rglob("*.v"),
                         *(out / "tb").glob("*.sv")]
                self.assertTrue(any(p.suffix == ".v" for p in files))
                artifacts = {str(p.relative_to(out)): (hashlib.sha256(p.read_bytes()).hexdigest(),
                                                       p.stat().st_mtime_ns) for p in files}
                stable = (cache["jit_cache_key"], cache["build_flags_hash"], artifacts)
                if index:
                    self.assertIn("jit-cache: hit", result.stdout)
                    self.assertEqual(stable, baseline)
                    self.assertEqual(cache["last_pycc_jobs"], 0 if index == 2 else len(module_manifests))
                    if index == 2:
                        self.assertEqual(cache["cpp_build_flags_hash"], previous_cpp_key)
                    else:
                        self.assertNotEqual(cache["cpp_build_flags_hash"], previous_cpp_key)
                else:
                    baseline = stable
                previous_cpp_key = cache["cpp_build_flags_hash"]
                run = subprocess.run([project["cpp_executable"]], cwd=out, env=env,
                                     text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
                self.assertEqual(run.returncode, 0, run.stdout)


if __name__ == "__main__":
    unittest.main()
