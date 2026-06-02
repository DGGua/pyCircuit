# C++ device hpp precompiled headers (`--cpp-pch`)

Optional CMake integration that records device module top-level `.hpp` paths in
`cpp_compile_manifest.json` so `gen_cmake_from_manifest.py` can emit
`target_precompile_headers`. This speeds up cold builds and incremental `.cpp`
recompiles when a design emits very large unoptimized headers.

PCH does **not** change C++ emit output; it only affects the CMake build path.

## Requirements

| Flag | Value | Notes |
|------|-------|-------|
| `--cpp-pch` | (flag) | Records PCH intent in manifest |
| `--cpp-split` | `module` | Required when PCH is on |
| `--emit` | `cpp` | Manifest written on C++ emission |

CLI build entry:

```bash
python3 -m pycircuit.cli build <design.py> --out-dir <dir> --target cpp \
  --cpp-pch
```

Direct `pycc`:

```bash
pycc design.pyc --emit=cpp --out-dir <dir> --cpp-split=module --cpp-pch
```

## Manifest

When enabled, `cpp_compile_manifest.json` includes:

- `profile_summary.cpp_pch`: `true`
- `precompile_headers`: absolute path(s) to `<module>/<module>.hpp`
- `precompile_headers_mode`: `"device_hpp"`

The project-level `cpp_project_manifest.json` aggregates `precompile_headers`
from per-module manifests (fallback: `flows/tools/cpp_pch_headers.py`).

## Orthogonal to member placement

`--cpp-pch` and `--cpp-localize-members` are independent bool flags and can be
combined (2×2 matrix: baseline/opt × PCH off/on). See
[cpp_member_placement.md](cpp_member_placement.md) for localization.

## When PCH helps

- Large unoptimized device headers (e.g. multi-MB `top.hpp`)
- Cold full builds where many TUs include the same header
- Incremental edits to `.cpp` only (header unchanged)

Less useful when headers are small (e.g. after `--cpp-localize-members`) or
regenerated on every `pycc` run.

## Scope

PCH applies only to CMake builds via `gen_cmake_from_manifest.py`. The direct
`build_cpp_manifest.py` compile path does not use precompiled headers.

## Gate

```bash
bash compiler/mlir/test/cpp_device_pch_smoke.sh
```
