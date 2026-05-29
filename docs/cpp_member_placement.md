# C++ comb wire member placement (`--cpp-localize-members`)

Emitter optimization that keeps struct members for ports, state, and cross-method
comb wires, while promoting single-method comb temporaries to function-local
`Wire<>` declarations. This reduces C++ struct size and downstream TU compile
cost for large hierarchical designs.

## Decision traceability

- Supports incremental / compile-scope goals in **Decision 0141** and stable
  emission artifacts in **Decision 0147** (`docs/rfcs/pyc4.0-decisions.md`).
- For `--emit=cpp`, `pycc` runs `pyc-cpp-placement`: records `pyc.cpp.comb_chunk_nodes` on the module
  (from `--cpp-shard-max-ast-nodes` / dev-fast / default 256), and when `--cpp-localize-members` also
  runs comb member localization (struct vs local).
- `CppEmitter` reads `pyc.cpp.comb_chunk_nodes` and placement attrs; it does not re-derive chunk size from CLI.

## Requirements

| Flag | Value | Notes |
|------|-------|-------|
| `--cpp-localize-members` | (flag) | Enables placement pass + localized emit |
| `--cpp-split` | `module` | Required when localization is on |
| `--emit` | `cpp` | Pass runs only for C++ emission |

CLI build entry:

```bash
python3 -m pycircuit.cli build <design.py> --out-dir <dir> --target cpp \
  --cpp-localize-members
```

Direct `pycc`:

```bash
pycc design.pyc --emit=cpp --out-dir <dir> --cpp-split=module --cpp-localize-members
```

## Manifest

When enabled, `cpp_compile_manifest.json` includes `cpp_placement` counts
(`struct_members`, `local_in_method`, `promoted_cross_method`, `probe_pinned_struct`)
under the profile summary.

## Gate

```bash
bash compiler/mlir/test/cpp_member_placement_smoke.sh
```

Evidence logs: `docs/gates/logs/<run-id>/` (see `docs/gates/README.md`).
