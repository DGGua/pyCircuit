# NewCircuit audit against the enabled GSIM source

The reproduction target is the enabled pipeline in the local `gsim/` snapshot,
not every technique mentioned in the DAC paper or every function present in
the source tree. The snapshot is a reference and must remain unchanged.
PYC decisions 0114/0115/0116/0121/0122 and 0127/0128/0134/0135 remain binding.

## Source and paper boundaries

`gsim/src/main.cpp:287-332` enables AST conversion, array splitting, loop
detection, topological sorting, width inference, dead-node removal, expression
simplification, used-bit analysis, bit splitting, constant analysis, aliases,
pattern detection, common expressions, partitioning, replication, statement
generation, instruction generation and C++ emission. Dead-node removal is
repeated between transformations. AST conversion also calls clock optimization
(`AST2Graph.cpp:1642`); reset grouping is called by partition coarsening
(`graphPartition.cpp:50-58`). These nested calls are part of the target.

| Mechanism | Enabled GSIM behavior and evidence | NewCircuit disposition and completion criterion |
| --- | --- | --- |
| Graph legality and width inference | `main.cpp:289-295` | PYC's typed, instance-aware MLIR legality gates own this. Do not import FIRRTL width inference or weaken cross-instance cycle/depth checks. |
| Dead nodes, aliases, constants and expression patterns | `deadNodes.cpp:55-81` roots outputs, special statements and external modules; `exprOpt.cpp:33-77` removes identical mux/when branches and redundant casts; `patternDetect.cpp:14-45,66-121` handles one-hot extraction and concat/shifted-OR equality | Hardware MLIR already owns the corresponding rewrites. Completion requires equivalence on legal PYC widths and value models, plus named-probe/state roots. Do not reproduce an unchecked source identity when it changes PYC values. |
| Array splitting | `splitArray.cpp:515-541` restricts optional splitting to ordinary/register arrays with fixed accesses; `splitOptionalArray` is called at line 446 | Fixed-index aggregate scalarization is applicable. Dynamic-index memory splitting is **not** evidence of missing source parity: this source explicitly rejects variable accesses. PYC memory timing and named observability remain intact. |
| Memory width demand | `usedBits.cpp:179-208` propagates reader demand into physical memory width and writer expressions | Every legal PYC memory requires a stable `name` (PYC942) and exposes complete storage through hash/watch/dump (Decision 0006). No legal anonymous, unobserved shape satisfies source narrowing's precondition. Preserve physical width even when hardware readers use only low bits; the array gate checks high-byte visibility and rejects anonymous memories. |
| Bit splitting | `splitNodes.cpp:809-810` excludes signed nodes and nodes feeding arrays; lines 822-845 split ordinary/register nodes and rewrite reset trees | Demand propagation alone does not reproduce independently activated segments. Require hardware-equivalent slices and separate execution activation for applicable unsigned scalar paths. General signed/dynamic aggregate splitting exceeds this source's scope. |
| Correlation coarsening | `mergeNodes.cpp:275-321,327-369,385-417`: one reverse out-degree-one pass, one forward in-degree-one pass, then siblings | Coarsened units must remain indivisible during initial partition. Out/in merges check the **recipient's existing** size (`>7000` rejects); sibling merges check recipient `<30`, with no bound on incoming size. A bound on the merged total changes source behavior. Gate the 7000/7001 and 29/30 boundaries and legal quotient ordering. |
| Initial partition cost and bound | `graphPartition.cpp:93-109`: interval outgoing supernode edges minus internal edges; strict `<` improvement preserves the first minimum. A first unit larger than the configured bound is still admitted alone | Default partition must operate on atomic coarse units and count distinct supernode edges. The bound limits adding further units; it does not split a coarse unit. A separate explicit strict-bound mode may retain NewCircuit's earlier partition behavior for experiments. Gate coarse units above the bound and a graph whose optimum differs from greedy packing. |
| Refinement and register merging | `graphPartition.cpp:383` comments out `graphRefine`; `main.cpp:325-327` comments out `mergeRegister` and `constructRegs` | **Not enabled; not completion requirements.** |
| Shared conditions and statement trees | `mergeNodes.cpp:155,205`, `StmtTree.cpp:362-427` group nested FIRRTL `when` trees, turn small cases into muxes, and build branch statements | PYC has explicit mux values, not FIRRTL last-connect `when`. Dependency-ready scheduling now groups late-ready scalar mux/select nodes and their inlined branch expressions. Graph/plan verifiers reject dependencies within a batch and batches spanning source effect boundaries. See the shared-condition gate below. |
| Common expressions / extraction | `commonExpr.cpp:99-159` compares whole existing ordinary scalar node trees; it merges when there are at least five duplicates, width exceeds `BASIC_WIDTH`, or a duplicate has multiple successors | This is selective whole-node CSE, not a general pass that extracts arbitrary repeated subtrees. MLIR CSE plus retained materialized shared SSA values cover the source mechanism. The paper's general extraction cost inequality is a broader target and must be labeled separately. |
| Replication | `replication.cpp:28-56,79-89` excludes aggregate results, `when`, and wide expression operators; only singleton supernodes have threshold 3, requiring recursive cost times distinct successor-node count `<3` | Reproduce scalar recursive expression costs, per-consumer-group copies and source removal. Dynamic/aggregate-result replication is not required by this source. Require an on/off value oracle and structural evidence that a copied source no longer executes. |
| Activity and packed checks | `cppEmitter.cpp:372-407,533-536,595-627`: scalar output-change propagation, unconditional writer/array successor activation, byte-wide outer checks; `786-793` makes external modules always active | Extend activity only where PYC's complete dependency/version set makes skipping valid. Effects, probes and external calls cannot generally be skipped just because explicit operands match. Packed masks may cross such nodes only when those nodes publish the required invalidation. Require unchanged-input repeated-tick, memory-version, clock-edge and effect-count oracles. |
| Activation cost model | `cppEmitter.cpp:382-389` selects branchless propagation when the number of bitmap writes is at most three; larger fanout uses one branch | A bitmap alone does not reproduce this choice. A graph/plan-owned fanout cost decision can select branchless masked stores versus a shared change branch without changing PYC values. Gate both fanout cases structurally and functionally. |
| Reset slow path | `mergeNodes.cpp:215-269` groups reset actions; `cppEmitter.cpp:635-703` emits grouped reset functions; `707-717` calls synchronous reset before substeps, while `544-585` handles async reset around its producer | The paper's Listing 6 describes cycle-end reset; the local source has a different schedule. PYC must compute next state and apply reset through transfer/commit, preserving TICK-OBS/XFER-OBS. A PYC slow path groups normal next-state work and a shared reset override, invalidating affected consumers at commit. Do not move committed reset effects into `comb()`. |
| Clock normalization | `AST2Graph.cpp:1642`, `clockOptimize.cpp:171-258` fold clock aliases/constants and translate recognized gated clocks into conditional updates | PYC clock-edge and CDC semantics are explicit. Only equivalent normalized cases apply; activity must retain actual edge/phase dependencies. |

## Acceptance contract

Completion is per mechanism, with three kinds of evidence: graph/plan structure
shows that the intended optimization happened; generated C++ agrees with an
independent functional oracle and optimization-disabled execution; applicable
stateful changes agree with Verilog at both observation points. A successful
compile or an isolated speedup is insufficient.

Source-equivalent scheduling does not mean copying source undefined behavior or
FIRRTL-specific semantics. In particular, keep pre-transfer memory reads,
transfer writes and old-data RDW (0114/0122), transfer-visible reset (0115),
X/Z-sensitive comparisons (0116 and later value-model decisions), observable
probe roots (0121), and instance-aware graph legality (0127/0128/0134/0135).
The coverage claim is source mechanism parity with PYC semantic adaptations.
It does not guarantee identical partitions or statement order: typed PYC clocks,
SSA muxes instead of FIRRTL last-connect `when`, and explicit effect boundaries
can produce different legal graphs and schedules.

The default partition policy preserves coarsened units. Tests that intentionally
force one-operation partitions must explicitly request the strict-bound policy;
they test downstream activation/replication behavior, not the GSIM bound rule.
`tests/newcircuit/run_partition_gate.sh` owns the default-policy structural and
functional checks. Large-design performance and full source coverage must not
be claimed until the corresponding cases and measurements have been run.

## Tested source adaptations and remaining coverage

* GSIM's `usedBits.cpp:179-208` narrows memory data width from reader demand.
  This source precondition does not hold for currently legal PYC memories:
  `CheckFrontendContractPass.cpp:353-369` requires a stable name on every
  byte/synchronous/dual-port memory, `pycc.cpp:860-900` registers each memory as
  an observable memory probe, and `runtime/cpp/pyc_sync_mem.hpp:74-102` exposes
  whole-storage hash/dump operations. High bits remain observable even when
  ordinary data readers truncate them. Retaining the full storage width is a
  required PYC adaptation, not a missing optimization. A future unobservable
  memory contract could permit narrowing, but cannot be inferred from unused
  data-port bits. Dynamic array splitting and memory width narrowing differ.
  `run_array_split_gate.sh` checks a 16-bit 1R1W memory and a 32-bit 2R1W
  memory whose ordinary readers observe only eight bits. Across 256 cycles,
  an independent host array model checks full storage, hash and dump before
  and after transfer, including high-byte writes, byte strobes, read holds,
  resets and same-address old-data reads; write-watch data retains high bytes.
  Verilog checks complete backing storage and all three low-byte outputs.
  IR assertions require the original widths, and removing a memory name must
  fail with PYC942. This validates the observability boundary; it does not
  implement memory-width narrowing.
* `mergeWhenNodes` delays ready conditions using dependency queues
  (`mergeNodes.cpp:104-155`). This can group nodes whose branch operands become
  ready after the first original conditional node. NewCircuit now uses ready
  queues at the pure-run and group-expression levels. The shared-condition gate
  covers interleaved late operands, two-operation inlined branch trees, mixed
  PYC mux/`arith.select`, dependent-mux rejection and an assertion barrier, under
  default, soft-bound-one, strict-bound and preserve-ops policies. Both the
  structural checks and independent C++ value oracle passed. FIRRTL last-connect
  and invalid-value semantics are not imported.
* GSIM's width-demand propagation is a graph-wide fixed point
  (`usedBits.cpp:173-205`); its bit splitter uses concatenation/reference segment
  boundaries (`splitNodes.cpp:741-757`). The shared MLIR greedy fixed point now
  normalizes partial scalar `trunc` readers through NOT/bitwise, casts, concat,
  mux/select, fixed shifts and registers. Partial demand crosses pure comb
  yields and block arguments without flattening the regions, including nested
  combs. Single partial register readers can
  narrow storage and next-state arithmetic; shared and overlapping readers
  retain shared segments. Full-width and explicitly observed roots remain
  barriers. The scalar-demand gates check actual resulting widths on shared
  DAGs, aliases, casts, muxes/selects and reset-fed state, plus independent
  C++/Verilog values. Source arithmetic components are conservative
  (`splitNodes.cpp:232-243`), and source signed/array exclusions remain explicit.
  This is evidence for the implemented paths, not a formal proof for every IR
  arrangement or an identity of the two compilers' partitions.
* Wide dynamic shift amounts normalize in shared MLIR before host conversion.
  A full-width range comparison contributes an overflow/unknown bit to a
  32-bit amount. Unlike a saturating mux with equal arms, this preserves X/Z
  when a high amount bit is unknown. The 64/128-bit amount gate checks known
  overshifts (including `1 << 32`) in normal and structural C++, and Verilog
  X/Z amounts with zero and nonzero data. Its independent oracle first checks
  the full amount against the operand width: this workspace's Icarus truncates
  wide amounts in procedural shifts. The gate also rejects diagnostic FATAL
  output even when VVP returns zero, and requires a final success marker.

The partition and shared-condition evidence is saved in
`docs/gates/logs/newcircuit-gsim-20260926/partition.log` and
`docs/gates/logs/newcircuit-gsim-20260926/shared-condition.log`. It establishes
these mechanisms, not unrestricted full-source equivalence. Clock alias/constant
optimization has no separate PYC representation to
port wholesale: PYC's typed clocks and explicit enable/CDC contracts first
determine which source cases are representable and equivalent.

## Direct reference differential check (2026-09-27)

`tests/newcircuit/run_gsim_reference_gate.sh` accepts an explicit `GSIM` binary
and runs equivalent hand-written FIRRTL and PYC designs through the same C++
oracle. Arithmetic wraparound, muxes, partial products, concat slices, repeated
inputs and selector-only changes passed 197,632 checks per simulator, with the
same digest `e4ccd8d72edf7475`. This is a small combinational differential check,
not a large-core benchmark or a cross-tool proof for stateful PYC adaptations.

No Clang >=19 was available. The reference compiler was built offline in a
`/tmp` copy using GCC 13.1.0. Only build policy changed: the Clang version pin
was disabled and `-Wno-error` allowed GCC diagnostics, including sign-comparison
warnings. Original
compiler/parser/header sources remained byte-identical. The generated model
contained no `_BitInt`; its GCC build explicitly bypassed the capability guard
with `-D__BITINT_MAXWIDTH__=64`. This is a **non-official toolchain result**,
limited to this small fixture. The optional gate defaults to Clang >=19 and
requires `GSIM_REFERENCE_ALLOW_GCC_U64=1` for the checked GCC fallback.
The reference tree in this repository was not changed.

The functional output is saved in
`docs/gates/logs/newcircuit-gsim-20260926/reference.log`; the matching FIRRTL,
PYC and shared oracle are stored beside the optional gate.
