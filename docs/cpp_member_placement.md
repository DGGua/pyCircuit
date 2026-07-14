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

## Reorder safety

`pyc.comb` is a pure, single-block SSA region. Its verifier rejects nested
regions and any body operation that is not memory-effect-free. Placement can
therefore change body order only by selecting another valid topological order;
stateful operations, `pyc.assign`, instances, memories, FIFOs, and assertions
are never admitted to this scheduler.

## Locality-aware scheduling and partitioning

Large comb regions no longer use fixed consecutive slices. Placement first
builds the SSA producer-consumer DAG and applies deterministic
shortest-completion-first Kahn scheduling. Among ready nodes it prefers the
smallest saturated downstream work estimate, then shorter remaining depth,
smaller live-weight growth, larger closed live weight, and finally original IR
order. Integer value weight is `1 + ceil(bit_width / 64)`.
If the resulting weighted cut is worse than the previous fixed-order split,
placement keeps the fixed-order schedule, so enabling the heuristic cannot
regress this metric.

The topology heuristic is deterministic but not globally optimal: finding the
minimum-frontier order over every legal DAG topological order is NP-hard. For
the minimum part count `K = ceil(node_count / chunk_nodes)`, dynamic programming
chooses boundaries that minimize the total weight of localizable values whose
definition and last use lie in different parts. Each such value is charged once
in its definition part, even when it crosses multiple boundaries.
`chunk_nodes` is a per-part size **cap** (TU budget), not a fill target: parts
may be shorter. With `slack = K * chunk_nodes - n` (`0 <= slack < chunk_nodes`),
the DP represents each part as `size = chunk_nodes - deficit` and enumerates
every feasible deficit in `[0, slack]`, so the boundary search is exact under
those constraints.

Placement writes the selected method on each comb operation and a stable comb
index on the wrapper. The C++ emitter consumes these attributes directly rather
than independently recreating fixed-size chunks.

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
under the profile summary. In addition to `struct_members`, `local_in_method`,
`promoted_cross_method`, and `probe_pinned_struct`, scheduling reports
`fixed_order_cross_method`, `scheduled_cross_method`, and
`scheduled_cut_weight`. The first two compare localizable cross-part values
before and after topology scheduling; the final field applies the width-aware
optimization weight.

## Gate

```bash
bash compiler/mlir/test/cpp_member_placement_smoke.sh
```

Evidence logs: `docs/gates/logs/<run-id>/` (see `docs/gates/README.md`).
