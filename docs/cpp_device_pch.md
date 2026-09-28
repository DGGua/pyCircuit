# C++ device hpp precompiled headers (`--cpp-pch`)

Optional CMake integration that records device module top-level `.hpp` paths in
`cpp_compile_manifest.json` so `gen_cmake_from_manifest.py` can emit
`target_precompile_headers`. This can speed up cold builds of very large
headers. PCH does not change C++ emit text.

## Requirements

| Flag | Value | Notes |
|------|-------|-------|
| `--cpp-pch` | (flag) | Records PCH intent in the manifest |
| `--cpp-split` | `module` | Required when PCH is on |
| `--emit` | `cpp` | Manifest is written on C++ emission |

```bash
python3 -m pycircuit.cli build <design.py> --out-dir <dir> --target cpp --cpp-pch
pycc design.pyc --emit=cpp --out-dir <dir> --cpp-split=module --cpp-pch
```

## Manifest

When enabled:

- `profile_summary.cpp_pch`: `true`
- `precompile_headers`: absolute path(s) to `<module>/<module>.hpp`
- `precompile_headers_mode`: `"device_hpp"`

`cli build` aggregates those headers into `cpp_project_manifest.json`
(fallback: `flows/tools/cpp_pch_headers.py`).

## Cache behavior

Toggling `cli build --cpp-pch` invalidates C++ device generation and its CMake
build. It preserves the JIT cache, Verilog generation, and testbench generation
when their inputs are unchanged. Repeating the same flag setting reuses all
unchanged backend outputs. The header selector is also included in packaged
wheels for manifest fallback handling.

The CLI collects source and header files from the current modules' compile
manifests. Obsolete unsplit files or numbered shards left in an output directory
are excluded from the next build; a missing active artifact reports an error.

PCH affects host compilation only. It preserves the NewCircuit simulation plan
and does not enable `--cpp-compile-budget`, which remains opt-in. Header
precompilation has its own cost, so measure the complete build for the intended
design rather than assuming a speedup.

## Gate

```bash
bash compiler/mlir/test/cpp_device_pch_smoke.sh
```

The smoke compiles and runs a generated counter with CMake PCH, checks invalid
flag combinations, and runs an off/on/on/off CLI build sequence with both
backends to verify cache isolation. Use `PYC_TOOLCHAIN_ROOT` when runtime headers
and libraries are staged separately from the selected `PYCC` binary.
The same-directory rebuild also retains deliberately invalid obsolete shards
to verify that the manifest, rather than a directory glob, controls compilation.
