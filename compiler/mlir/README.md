# `compiler/mlir`: MLIR dialect + tools (prototype)

This folder contains the MLIR-based implementation of the `pyc` dialect, along with:

- `pyc-opt`: `mlir-opt`-style tool with `pyc` dialect + passes
- `pycc`: compile `.pyc` (MLIR) to Verilog or C++ via template libraries

## Build

Recommended: build from the repo root via top-level `CMakeLists.txt` (see `README.md`).

You can also build this subproject standalone if you already have an LLVM+MLIR build/install.

This example assumes an existing LLVM/MLIR 19 install or build tree.

```bash
cmake -G Ninja -S compiler/mlir -B /tmp/pyc-mlir-build \
  -DMLIR_DIR=/path/to/llvm-19/lib/cmake/mlir \
  -DLLVM_DIR=/path/to/llvm-19/lib/cmake/llvm

ninja -C /tmp/pyc-mlir-build pyc-opt pycc
```

## Passes (prototype)

### `pyc-check-wire-drivers`

Enforces the dialect-level connectivity contract before wire elimination can
erase the evidence. `pyc.wire` is single-driver SSA/backedge plumbing, not an
implicit resolved Verilog net: every multi-driver wire is rejected, as is an
undriven wire that is read, named, or explicitly debug-kept. A legal late
single driver is accepted. Unnamed, unread placeholders may be removed as dead
IR; named single-driver wires remain storage-backed observation roots in both
emitters.

### `pyc-eliminate-wires`

Eliminates trivial `pyc.wire` + `pyc.assign` pairs when safe (single driver that
dominates all reads), and removes dead wires. This reduces netlist noise and
helps subsequent CSE/constprop. Named/debug-kept wires without SSA readers are
retained together with their driver so ProbeRegistry and Verilog observe the
same value.

`pycc` runs this pass by default before emission.

### `pyc-comb-canonicalize`

Combinational simplifications, currently focused on mux canonicalization:

- collapses nested muxes with the same select
- rewrites some `i1` mux patterns into simpler boolean logic

`pycc` runs this pass by default before emission.

### `pyc-fuse-comb`

Fuses consecutive pure combinational ops (`pyc.add/mux/and/or/xor/not/constant`) into
`pyc.comb` regions. This is a codegen-oriented transform intended to enable:

- flattened Verilog emission (`assign` instead of many tiny module instantiations)
- inlined C++ combinational evaluation (fewer tiny objects / calls)

This is the legacy `--comb-partition=none` path. The default
`--comb-partition=static` path deliberately skips this pass so temporary fused
region boundaries cannot bias the unified plan. It is also skipped by
`--sim-mode=cpp-only --cpp-only-preserve-ops`.

### `pyc-partition-comb`

Builds the canonical value-level `CombDepGraph` for each non-structural
function, projects memoizable operations from that graph, applies the
GSIM-style `mergeOut1`, `mergeIn1`, and `mergeSiblings` coarsening phases, and
uses a stable-topological contiguous DP to respect `max-nodes`. It directly
emits the final `gsim-unified-v2` sibling `pyc.comb` runtime units; it does not
partition an earlier FuseComb result.

This is a module pass because instance edges need a stable caller/callee view.
Its three phases are:

1. Read-only preflight every function against the original module. Once every
   graph succeeds, the A-to-B normalization step transparently unfolds valid
   pre-existing comb regions across the module.
2. Build and freeze every function plan against the same immutable unfolded
   module, without rewriting IR.
3. Materialize all final sibling comb regions from the frozen plans in one
   rewrite phase.

The cycle checker, logic-depth checker, and partitioner all use the same
`CombDepGraph` semantics. An instance or asynchronous primitive can be a
physical placement barrier without being a same-tick semantic cut; sequential
state is a cut. Declaration-only callees in split builds therefore require an
exact validated `pyc.comb_dep_summary.v1` produced from the full design.
Graph construction and summary/depth propagation consume one shared
per-result transfer resolver (operand dependencies, edge kind, base depth, and
edge cost). A result-producing operation without a registered transfer fails
closed instead of receiving a conservative all-input/all-output guess.

`pycc` runs this pass by default through `--comb-partition=static`; use
`--comb-partition=none` to retain the legacy FuseComb behavior. The surrounding
`pyc-check-comb-memoizable` and `pyc-check-comb-partitions` gates validate both
incoming and final plans.

The C++ `--comb-update=dirty` policy consumes these final partitions with a
static topological scan. Inactive units take a fast return; a producer whose
outputs do not change neither stores those outputs nor activates direct
fanout. Boundary inputs use exact snapshots. This is not a dynamic work queue.

### `pyc-check-flat-types`

Verifies that the IR is fully lowered to emission-supported hardware types:
integers, recursively nested vectors of integers, `!pyc.clock`, and
`!pyc.reset`. This rejects unsupported aggregate or dynamic types before either
backend sees them while retaining PYC's native vector representation.

`pycc` runs this check by default.

### `pyc-prune-ports`

Module-level cleanup pass that prunes unused `func.func` arguments and updates
`func.call` sites. This changes the externally visible interface, so it is
**not** run by default in `pycc`, but can be useful for internal
refactors or design-space exploration flows.

## Per-pass IR dump (diagnostics)

`pycc` and `pyc-opt` can write the IR before and/or after every pass to a
directory so the effect of any single pass is directly diffable. This is a
diagnostics-only feature: it never modifies the IR and is disabled by default
(zero overhead when not requested).

```bash
pycc foo.pyc --emit=none --dump-pass-ir=/tmp/pir
ls /tmp/pir
# 0000_before_0001_check-frontend-contract__L0.mlir
# 0001_after_0001_check-frontend-contract__L0.mlir
# 0002_before_0002_inline-functions__L0.mlir
# ...

# Diff the IR across a specific pass:
diff /tmp/pir/0046_before_*eliminate-wires* /tmp/pir/0047_after_*eliminate-wires*
```

File names are `NNNN_<before|after>_<NN>_<pass>__L<level>[_FAILED].mlir`, so
lexical order matches execution order, before/after of one pass share the same
`<NN>`, `__L0` is a module-level pass and `__L1` is func-nested, and a failed
pass gets a `_FAILED` suffix (the file begins with `// PASS FAILED`).

Flags:

| Flag | Default | Purpose |
|------|---------|---------|
| `--dump-pass-ir=<dir>` | (empty = off) | Output directory. On `pycc`, `auto` means `<--out-dir>/pass_ir`. `pyc-opt` requires an explicit directory (`auto` is rejected). |
| `--dump-pass-ir-phase=before\|after\|both` | `both` | Which phase(s) to record. |
| `--dump-pass-ir-filter=<regex>` | (empty = all) | ECMAScript-style regex on the pass short name (e.g. `eliminate-wires|fuse-comb`). |
| `--dump-pass-ir-max-lines=<N>` | `0` (unlimited) | Truncate each file after N lines (appends `// truncated at N lines`). |

The instrumentation coexists with `--profile-pass-timing` / `--profile-json`; the
two instrumentations are independent and can be enabled together.
