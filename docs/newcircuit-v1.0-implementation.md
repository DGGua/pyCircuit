# NewCircuit v1.0 implementation record

NewCircuit v1.0 is the C++ simulation architecture refactor based on pyCircuit
v4.0. This document records the current implementation, not a release claim.
Existing `pycircuit`, `pycc`, PYC IR, runtime, license, and Verilog interfaces
remain in use.

## Starting point

- Branch: `newpyc`; starting commit: `642def3c46e8890b5c91f3700068df0114455993`.
- `main` resolved to the same commit at the start. It was read only.
- Pre-existing untracked paths were `.vscode/`, `docs/fusecomb-vs-static-comb.html`,
  `docs/gsim-style-comb-partition.html`, three RFC drafts under `docs/rfcs/`,
  `gsim/`, and `p2sim/`. This work did not edit them.
- Compiler toolchain: LLVM/MLIR 19 from
  `/home/lys/pyCircuit/.pycircuit_out/deps/llvm19/root/usr/lib/llvm-19`;
  GNU C++ 13.1.0. The repository's existing CMake cache initially lacked
  `MLIR_DIR`; it was configured against that toolchain before building.
- A separate `/tmp/newcircuit-head-src` archive and build of the starting
  commit served as the source matched reference. The older installed `pycc`
  binary generated different C++ from that commit, so it was not used for
  source matched comparisons.

## Layers and ownership

The C++ path now has four explicit representation boundaries:

```text
Legalized PYC IR
  -> SimulationAST (hierarchical operations, regions and block arguments)
  -> SimGraph (nodes, value dependencies and state/observable boundaries)
  -> SimGraph passes -> optimized SimGraph
  -> SimulationPlan (evaluation order, cache and iteration decisions)
  -> CppEmitter -> generated C++
```

The AST and graph stages follow the separation of parse tree, graph, graph
transforms and C++ emission visible in
[GSIM's compiler pipeline](https://github.com/OpenXiangShan/gsim/blob/master/src/main.cpp).
NewCircuit retains PYC semantics, its two-phase runtime and its own
SimulationPlan. The graph is a dependency network: combinational portions may
be DAGs, while stateful feedback and instance evaluation can require SCC
handling. The GSIM port now includes Hardware MLIR value and state rewrites,
SimGraph partitioning, replication, inlining and activity propagation. The
remaining coverage is tracked
in `docs/rfcs/newcircuit-gsim-graph-optimization.md`.

| Existing responsibility | Current owner | Rule and condition |
|---|---|---|
| Hardware lowering, simplification, state cleanup, type and dynamic gates, combination cycle and depth checks | Hardware MLIR pipeline in `pycc` | Existing pass order and gates remain for each output path except C++ simulation fusion. Decisions 0127, 0133 to 0136 remain the hardware contract. |
| Consecutive combinational fusion for C++ | `runConsecutiveCombGroupingPass` in the SimGraph pass stage | Same eligible op set, consecutive run, at least two ops, live output, structural opt out, and `--cpp-only-preserve-ops` condition as the prior pass. It operates after hardware cleanup; exact group boundaries can differ from the old pre-cleanup pass. |
| Existing `pyc.comb` fusion for Verilog | Original `pyc-fuse-comb` MLIR pass | Pass selection, position, and Verilog emission are unchanged. |
| Value dependencies, assignment producers and state/observable boundaries | AST to SimGraph builder | Graph nodes retain all results and operands, including nested `pyc.comb` operations in `fineNodes`. Multiple drivers are marked so the planner retains the prior fallback. |
| Topological and fallback scheduling | Simulation planner | Consumes optimized graph edges; no dependency reconstruction from PYC operands occurs in the planner. |
| SCC worklist order, cyclic iteration limit and bounded fallback | Simulation planner | Existing Tarjan, component ordering, primitive count, and limit formulas are retained. |
| Instance and primitive cache representation and commit invalidation | Simulation planner | Packed words at 12 operands or 16 packed words; narrow inputs use fingerprints at 64 bits or less. The AST marks stateful callees before graph construction and the plan invalidates caches on commit, including recursive hierarchy conservatively. |
| Primitive, instance, combination, and module order; step phases; helper chunk sizes | Simulation plan | Existing deterministic name keys, module dependency order, two phase state update and configured chunk size are recorded before emission. |
| C++ syntax, declarations, expressions, runtime calls, probes and formatting | C++ emitter | It receives a verified plan, emits its selected groups and order, and rejects an incomplete plan. |

`buildSimulationAST` captures each legalized function without scheduling or
choosing a C++ strategy. It keeps nested regions and block arguments visible,
and freezes instance port paths, state boundaries, function ports and execution
metadata before graph construction. It also freezes value types, use counts,
observability, naming hints and declaration order; the graph imports these
facts from the AST.
`buildSimGraph` consumes that AST and constructs top-level value-dependency
edges, including assignment producers, state boundaries and observable uses.
`runSimGraphPasses` verifies the input graph, applies combinational grouping,
bounded partition, exact used-bit activation and group input-change activation,
then verifies the optimized SimGraph before
scheduling begins. Hardware-level dead-value removal stays in the MLIR pass
pipeline. The graph verifier checks that the stored dependency set is complete.
`--sim-group-activation=false` disables the activation pass for equivalence
and performance comparisons. `--sim-used-bit-activation=false` keeps full
input comparisons; `--sim-supernode-max-size=0` disables bounded partitioning.
The remaining GSIM scope is recorded in
`docs/rfcs/newcircuit-gsim-graph-optimization.md`.

`SimGraph` borrows MLIR operations and values and must be built and consumed
before the legalized PYC module changes. Its top nodes can be grouped without
mutating PYC IR; its fine nodes keep the original operations visible. The
`buildSimulationPlan` entry point accepts only the optimized graph. The
`SimulationPlan` owns the ordered operation references, cache decisions, SCC
components, fallback iteration limit, phase order, and helper sizes. These are
structured data, not generated C++ text. Direct `--emit=cpp`, `--out-dir`
module split, and `--cpp-split=none` all build plans before invoking the
emitter.

The existing `pycc` translation unit sharding step still uses generated C++
line and byte counts to package output files. It does not schedule simulation
operations. The emitted C++ still contains runtime cache checks and loops to
execute the selected plan.

## Verification performed

| Check | Result |
|---|---|
| Baseline calculator build and testbench with previously installed toolchain | Built; testbench printed `OK`. The binary was older than starting source. |
| `ninja -C .pycircuit_out/toolchain/build pycc` with LLVM/MLIR 19 | Passed. |
| Starting commit versus new compiler, calculator C++ with fusion disabled | Generated C++ was byte identical. |
| Starting commit versus new compiler, calculator Verilog with fusion enabled | Generated Verilog was byte identical. |
| Input containing existing `pyc.comb` regions, captured from the starting compiler's fusion pass | Accepted by the new C++ path; existing comb helpers were emitted. |
| New C++ builds and testbenches: calculator, `hier_modules`, `fifo_loopback`, `mem_rdw_olddata`, `multiclock_regs` | All built and printed `OK`. Starting commit builds and testbenches for hierarchy, FIFO, and memory also printed `OK`. |
| `tests/newcircuit/run_simulation_plan_gate.sh` | Passed: default grouping, grouping opt outs, nested `pyc.comb` AST/graph execution, runtime testbench, `--cpp-split=none`, and both SCC worklist and bounded fallback on stateful instance feedback. |
| Forced C++ module sharding at 50 lines / 2,000 bytes / 2 AST nodes | Produced 10 source shards; `build_cpp_manifest.py` compiled them with the calculator testbench, which printed `OK`. |
| `flows/scripts/run_semantic_regressions_v40.sh` with `CCACHE_DISABLE=1` and the rebuilt `pycc` | Passed after cache invalidation planning: X/Z, reset/invalidation order, and net resolution/depth across C++ and Verilator; summary at `docs/gates/logs/newcircuit-20260923-cache-plan/semantic_regressions_summary.json`. |
| `flows/tools/check_api_hygiene.py` over frontend, examples, docs and README | Passed. |
| `git diff --check` | Passed. |

The AST/Graph boundary change was additionally checked with isolated C++ and
Verilator builds for `xz_value_model_smoke`, `reset_invalidate_order_smoke`,
and `net_resolution_depth_smoke`; all three built and ran successfully. For
the net resolution design with combination grouping disabled, the starting
compiler and current compiler emitted byte-identical direct C++.

For the same calculator IR, the starting source generated 426 lines / 16,238
bytes of direct C++; the current path generated 382 lines / 14,893 bytes.
This is a code size observation, not a simulation speed claim.

These checks cover representative behavior and output modes. They do not prove
equivalence for every input or a performance target. The SCC worklist and
bounded fixed point paths ran on a stateful instance feedback fixture, but the
fixture is small. Existing input `pyc.comb` regions retain their container
execution boundary for compatibility, while fine grained internal operations
are kept in the graph. Broader performance and design regressions remain to be
run before NewCircuit v1.0 can be called complete.
