# NewCircuit: GSIM optimization ownership and coverage

Status: partial implementation. The Hardware MLIR pipeline covers
some value-level mechanisms, including GSIM's concat-equality, the disjoint
shifted-OR equality variant, one-hot shift-extraction patterns, narrowing
of single-use add/sub/mul and fixed shifts under an observed bit slice, and
shared narrowing of add/sub/mul, mux and fixed shifts with multiple slice
readers, splitting a wide
bitwise value into shared segments across disjoint or overlapping slice
consumers, splitting a scalar mux at separated observed slices while sharing
overlapping segments, mapping partial `trunc`, `zext` and `sext` readers to
source bits or extension fill, and recomposing a slice that
spans several concatenation fields. An unnamed scalar register observed only
through slices is split at slice boundaries before both backends; overlapping
readers share the common state segment. A vector register read only at fixed
outer indices is split recursively into observed scalar lanes, including
multi-dimensional vectors. Elementwise vector expressions with only fixed-index
readers are likewise lowered through each dimension to observed scalar lanes.
The supported operations now include arithmetic and remainder, bitwise logic,
comparisons, scalar-condition `arith.select`, width casts, extracts and
fixed/dynamic shifts; all readers of an
expression are rewritten in one pass and identical lane readers share a
scalar result. Fixed-index vector state and expression splitting no longer
has a fixed 64-reader cap. Low-width truncations narrow modular add/sub/mul.
Fixed-index reads of `v_broadcast_dim` now select a source lane or a smaller
broadcast before either backend. Multi-reader constant-amount dynamic shifts become fixed
shifts in Hardware MLIR. Multi-reader right shifts form an input slice with
zero or sign extension when the demanded prefix reaches the fill region.
Rank-2 vector reductions observed through only selected output indices now
reduce just those rows or columns in Hardware MLIR, for OR, AND and modular
addition in both chain and tree modes.
The selected-column reduction also exposes only the needed scalar cells of
an unnamed rank-2 vector register through the existing register lane splitter.
SimGraph captures typed pure expressions, including nested `pyc.comb` bodies,
as graph-owned opcode/operand/attribute descriptions. Each `pyc.comb` region
also has graph-owned argument, step and yield IDs, including recursive region
IDs for nested `pyc.comb`; the dialect verifier rejects effectful body steps.
SimulationAST first freezes every supported pure expression's opcode, constant,
index or shift immediate, reduction dimension and reduction mode. SimGraph
imports those descriptions instead of rereading pure-operation semantics from
MLIR during graph construction. It also records every node's execution kind,
primitive depth and name, instance symbol and display names, CDC stages and
assertion message before graph construction. It also resolves instance port
paths, records whether the callee contains state, and freezes function port
paths and structural-emission mode. Graph construction imports nested
`pyc.comb` arguments, steps and yields from AST regions and imports expression
operands and results from AST nodes. The AST still borrows source operations
and values for structural verification and signal identity. It snapshots value
types, use counts, observability, naming hints and declaration order; graph
construction imports these facts instead of querying source values again.
The C++ emitter now dispatches every supported pure operation through
`SimExpr`, including vector dimension broadcasts; the old MLIR operation
dispatch for pure expressions has been removed.
Nested comb helper generation now takes planned region and node IDs directly.
SimulationPlan also fixes step chunks for every comb region, including nested
regions, and verifies the chunk sequence before emission.
Register construction and tick bindings now use graph value IDs and captured
types. Graph-owned output roots and named probe IDs feed a SimulationPlan map
from alias/comb passthrough values to local register outputs; the C++ emitter
uses that map for probe registration.
CDC synchronizer stage counts, inputs and outputs are captured and verified in
SimGraph, then used for constructor and tick emission.
Single-clock FIFO depth, data width, port bindings and cache inputs follow the
same path; its emitted C++ remains byte-for-byte identical on the focused
fixture.
Dual-clock FIFO depth and bindings are likewise captured in SimGraph; the
focused generated C++ is byte-for-byte identical to the prior path.
All memory node kinds capture depth and optional instance name in SimGraph.
All three memory families now use graph value IDs for C++ port bindings,
constructors, probe registration and tick calls. The byte memory's eval cache
inputs also use graph IDs. Focused generated C++ is byte-for-byte identical to
the prior path for each family.
Layered instance nodes capture callee and display names, plus the callee's
input and output port paths. The module plan uses those graph nodes to order
callees before callers. Instance port matching is verified at graph build;
unique C++ port names are fixed in SimulationPlan from captured paths. The emitter reads the
planned ports, graph value IDs and cache decisions for instance evaluation and
tick calls. SimulationPlan also fixes instance tick-compute/commit chunks.
The initial graph also captures input/output port paths, input IDs and result
declaration order. The emitter declares ports, internal signals and group cache
inputs from graph value types rather than walking source operations for these
declarations.
Signal name bases and explicit-name flags are captured in the graph. The C++
emitter's naming table now allocates names from graph value IDs and those
captured hints; it does not inspect PYC MLIR operations or types. The graph
also captures the function symbol and a source-backed flag for each value,
which the emitter uses in place of MLIR handles. Borrowed source handles
remain for graph verification and diagnostics.
Graph nodes also carry a verified execution category. Pure graph grouping,
dependency scheduling, operation ordering, plan validation and the eval
dispatch consume that captured category.
SimGraph Passes read captured expression kinds, value IDs, source-backed flags
and the structural-emission setting; they no longer inspect MLIR operation
attributes or result lists while choosing graph rewrites.
SimulationPlan now allocates scheduling keys from graph value IDs and captured
operation names, derives group-propagation mask widths from graph types, and
orders modules by captured function names. Source handles remain for boundary
verification and diagnostics.
SimulationPlan also records one verified execution action per graph node:
assignment, assertion, pure expression, comb-region call, group call, primitive
call or skip. The eval and combinational C++ emit paths render these planned
actions instead of selecting behavior from MLIR operations or graph node kinds.
The plan also fixes fallback primitive evaluation chunks and local tick-compute
and tick-commit action sequences. The emitter renders these verified sequences;
the register reset-group action shares one clock edge and reset check across
registers with the same controls.
It forms dependency
components within pure runs, coarsens the execution DAG using GSIM's
out-degree-one, in-degree-one and equal-predecessor sibling rules with quotient
cycle checks, splits oversized components and runs a bounded global initial
partition over consecutive pure DAG nodes, inlines
cheap single-use scalar expressions, replicates a cheap singleton expression
into distinct consumer groups when its standalone evaluation can be removed,
including one-use sources, repeated uses in the same target group and a
two-operation scalar fragment formed by successive copies. After copying,
the empty source execution node is removed from the DAG. Same-group inlining
covers scalar arithmetic, comparisons and fixed-index reads while bounding
the expression tree to two modeled host operations. It groups at least six
independent mux nodes with a common scalar selector before
partition when their operands are available at the first mux, then marks
large mux runs for one C++ condition branch, moving independent expressions
between the muxes after that branch,
and propagates exact scalar bit demand backward across partitions for
activation, including bits fixed by constant AND/OR operands and through
nested pure `pyc.comb` argument/yield bindings and three dynamic-shift opcodes
when their shift amount is constant or dynamic. Dynamic amounts use a
logarithmic bit-mask dilation over all possible shift distances. Expensive
remaining pure singleton expressions also receive activity checks, excluding
constants and aliases whose comparison would cost more than assignment.
A packed activity bitmap propagates demanded output-bit changes between
topologically ordered pure groups whose inputs all come from other groups.
For fixed-index vector reads, activation and packed propagation compare only
the observed outer lanes. When a rank-2 input is read through fixed row and
column indices, activity and packed propagation compare just the observed
elements. The group input cache also updates only those selected lanes or
elements when the group runs. Other vector operations still use full-input
comparisons.
The remaining GSIM mechanisms below are not yet reproduced.

This request extends the older NewCircuit v1.0 architecture-only scope.
Preserve PYC hardware semantics, module SimObject boundaries, and Decision
0127/0128/0134/0135 legality rules. The references are GSIM's `src/main.cpp`
pipeline and the DAC 2025 paper. The local `gsim/` snapshot is read-only.

## Stage ownership

The dividing rule is the effect of a transformation. A pass that changes a
hardware value, width, state object, alias, constant, or observable root belongs
to Hardware MLIR Passes and must be verified there for both C++ and Verilog.
A pass that changes only evaluation granularity, activity, replication for
execution, or scheduling belongs to SimGraph Passes or SimulationPlan. Some
GSIM source functions combine both concerns and need more than one NewCircuit
pass.

| GSIM stage or mechanism | NewCircuit owner | Current coverage | Missing work |
|---|---|---|---|
| `splitArray`, width inference | Hardware MLIR | Typed PYC IR, optional `VectorUnroll`, flat-type gate, recursive fixed-index lane splitting for vector registers and observed elementwise expressions, including arithmetic/remainder, bitwise logic, comparisons, scalar-condition selects, casts, extracts and shifts; fixed-index reads through dimension broadcasts and selected output lanes of rank-2 OR/AND/add reductions; no fixed reader cap | General aggregate splitting for memory shapes, dynamic indices and observable probes. |
| `detectLoop` | Hardware MLIR legality | `CheckCombCycles`, including hierarchy | Keep its gate before graph building. |
| `removeDeadNodes`, dead registers and aliases | Hardware MLIR | Canonicalizer, CSE, dead-instance/state and wire elimination; `RemoveDeadValues` where supported | Audit named probes, observability and all state roots against GSIM cases. |
| `constantAnalysis`, value-level `exprOpt`, `aliasAnalysis`, `patternDetect` | Hardware MLIR | SCCP, canonicalizer, `CombCanonicalize`, `EliminateWires`; concat-equality, disjoint shifted-OR equality and one-hot extraction | Extend the patterns to the remaining legal shapes and identities with PYC width, reset and value-model proofs. |
| `commonExpr` for value equivalence | Hardware MLIR | CSE | Add guarded extraction or materialization rules after cost analysis. |
| `usedBits`, value-level `splitNodes` | Hardware MLIR | Vector canonicalization/unrolling; extraction and truncation pushed through bitwise NOT, single and multiple concatenation fields; partial readers of `trunc`, `zext` and `sext` mapped to source bits or fill; shared bitwise and scalar mux segments for overlapping slice readers; single-use and multi-reader narrow add/sub/mul and mux; fixed left/logical-right/arithmetic-right shifts including zero and sign fill for both single and multiple readers; multi-reader constant-amount dynamic shifts canonicalized first; unnamed scalar registers split at every observed slice boundary, including overlapping readers; bitwise/state slice splitting uses a boundary sweep without a fixed reader-count cap; recursive fixed-index vector register lanes and fixed-index `v_broadcast_dim` reads | General array segment splitting and register cases with observable-probe handling. |
| `usedBits`, activity-level `splitNodes` | SimGraph Passes | Backward exact bit demand across captured expressions, nested pure comb regions and partitions narrows input-change checks | Split graph nodes whose bit ranges activate independently. |
| Cost-driven execution inlining and extraction | SimGraph Passes | Graph-owned pure expressions; scalar arithmetic, comparisons, fixed-index reads, casts and bitwise expressions inline when all readers stay in one group, their modeled repeated evaluation costs no more than one evaluation plus materialization, and the resulting inline tree has at most two modeled operations. This includes two-reader one-operation expressions and zero-cost aliases/constants with more readers | Add broader operation and host cost model, materialization/extraction and cross-group graph rewrites. |
| `graphPartition` supernode coarsening and bounded partition | SimGraph Passes | Dependency components within pure runs; out-degree-one, in-degree-one and sibling coarsening across unrelated `pyc.comb` regions; bounded dynamic programming within an oversized component and over consecutive pure graph nodes, with effectful cuts; independent common-selector scalar mux nodes are grouped before partition and runs of at least six emit one branch even when unrelated expressions separate them | General shared-condition expression trees and exact source cost/bounds. GSIM's `graphRefine` function exists but is not invoked in its published pipeline. PYC has mux expressions but no FIRRTL `when` node. |
| `replicationOpt` for host execution | SimGraph Passes | Scalar singleton expressions up to 64 bits, including arithmetic, comparisons and fixed-index reads from materialized vector inputs, are copied into pure consumer groups when GSIM's singleton cost threshold permits; a source with one use or several uses in one group creates one copy, and an empty source execution node is removed. Previously copied scalar dependencies can be copied recursively within the same cost bound. Upstream pure values may be used if their source computation stays materialized | Extend to general compound DAG fragments, dynamic/aggregate array expressions and the full recursive expression cost rule. |
| Active-bit propagation and packed checks | SimGraph Passes, SimulationPlan and runtime | Group input-change activation, exact demanded-bit comparisons, fixed-index outer-vector-lane and rank-2 element comparisons for cache checks and packed propagation, pure singleton activation except constants/aliases, packed activity bits for topological pure-group dependencies, demanded-bit output-change propagation, existing instance/primitive caches, and planned batches of registers sharing clock/reset that check the edge and reset once | Activity for effectful nodes, packed checks across effectful boundaries and a full GSIM reset slow path. |
| `topoSort`, `generateStmtTree`, `instsGenerator` | SimulationPlan lowering | SimulationAST freezes pure-expression semantics, effectful-node execution metadata, instance port paths/state classification, function ports and nested comb-region bindings before graph import; dependency order, SCC fallback, phase and cache plans are keyed by graph node/value IDs; assignment and assertion execution data, register, CDC, both FIFO, all memory and instance bindings, output roots and named probes are captured in SimGraph; pure groups and comb regions lower to verified planned chunks; assignment, assertion, pure-expression, comb/group-call and primitive-call actions are verified per node; fallback primitive evaluation chunks and local tick-compute/commit sequences are planned and verified; instance port names and register probe passthroughs are resolved in the plan | Lower remaining names through graph-owned descriptions and build complete statement trees for primitive internals and tick phases. |
| `cppEmitter` | C++ emitter | Emits planned order and group checks; all supported pure operations use graph-owned expression semantics, while eval dispatch renders verified per-node SimulationPlan actions | Lower primitive internals and remaining names without rediscovering MLIR semantics. |

## Representation needed for full coverage

`SimGraph` now owns pure expression opcode, operand IDs, widths, constants and
attributes. Its nodes and edges carry graph value IDs, explicit expression
users and DAG validation; the replication pass creates synthetic expression
and value IDs without changing PYC IR. The C++ emitter generates top-level and
nested pure expressions from these descriptions. SimulationPlan stores
execution and SCC orders as graph node IDs. The graph still borrows MLIR
`Operation *` and `Value` for construction-time verification, use counts and
diagnostics. C++ signal names, expression semantics, effectful bindings and
instance ports are captured before emission. Assignment
sources/targets and assertion conditions/messages are captured as graph-owned
execution data. All supported steps inside `pyc.comb` use graph expression or
nested-region IDs. Planning decisions
for order, activity and caches now use graph node/value IDs.
Full GSIM coverage requires:

1. Stable graph identities for signals and expressions, with opcode, width,
   signedness, operands, constants, slices, and source location. Graph rewrites
   must redirect uses without mutating legalized PYC IR.
2. Explicit current-state and next-state boundaries, memory/FIFO/CDC effects,
   instance boundaries, assertions, named probes and output roots. The MLIR
   `CombDepGraph` remains the hardware legality authority.
3. A graph-native `SimulationPlan` containing expression order, partitions,
   per-bit activity, cache inputs, phase decisions and statements for every
   effectful operation. Pure groups already lower to planned statements; the
   remaining effectful paths still need this representation.
4. Graph verification before and after every pass: complete use-def edges,
   widths/slices, acyclicity at combinational cuts, roots and hierarchy safety.

## Verification contract

- For each behavior-changing Hardware MLIR pass, compare both C++ and Verilog
  against the same legalized design, including named probes, reset, memory,
  nested `pyc.comb`, cross-instance feedback and wide vector slices.
- For every SimGraph pass, compare optimized and disabled simulation over
  multiple cycles and assert the intended graph transformation or activation
  count occurred. Output equality alone does not prove coverage.
- Track compile time, generated C++ size, simulator throughput and activation
  counts on identical large designs before claiming GSIM-level performance.

## Gate evidence (2026-09-24)

- `ninja -C .pycircuit_out/toolchain/build -j 4 pycc`: passed.
- `tests/newcircuit/run_simulation_plan_gate.sh`: passed, including scalar,
  sparse-bit and vector group activation, bounded partition, IR-dump checks,
  C++/Verilator samples for hardware rewrites and an Icarus X-state equality
  case for shifted-OR splitting.
- After the AST value snapshot, the full gate passed with nested `pyc.comb`,
  stateful hierarchy, named probes and the generated 20,000-operation DAG.
- Gapped scalar mux slices rewrite one 24-bit mux into four shared 4-bit
  segments. IR shape checks, 128 random C++ cases and 128 Icarus Verilog
  cases passed.
- Partial readers of `trunc`, `zext` and `sext` remove the original wide casts;
  input slices, narrow sign fill and zero constants replace them. IR shape
  checks, 256 random C++ cases and 256 Icarus Verilog cases passed.
- A generated 257-instance chain crosses the plan's 256-instance tick chunk
  boundary; both compute and commit helpers contain exactly 257 submodule
  calls, and the generated C++ passes a C++17 syntax check.
- Isolated C++ and Verilator builds and testbenches for
  `xz_value_model_smoke`, `reset_invalidate_order_smoke`, and
  `mem_rdw_olddata`: passed with group activation enabled.
- Rebuilt and ran the `reset_invalidate_order_smoke` C++ testbench after the
  exact-bit activation change: passed.
- C++ equivalence gates cover DAG coarsening across an unrelated comb region,
  sibling coarsening, replication, bit-demand propagation and a 20,000-op deep
  legal DAG. The latter compiles in about 0.6 seconds after iterative clock
  domain and logic depth analyses replaced recursive traversal.
- A later 20,000-add emission check took 0.84 s with graph replication and
  0.82 s with it disabled on this workspace; both generated 1,857,664-byte
  C++ files. A C++17 syntax check of the enabled file took 15.83 s and about
  301 MB peak RSS. These are single-run checks, not throughput results.
- After singleton activation widened, the same 20,000-add fixture emitted in
  0.80 s with activity enabled and 0.82 s with it disabled. Generated C++ was
  1,857,664 versus 1,584,182 bytes, respectively. This is still a synthetic
  design and does not establish simulation throughput.
- The repository's V5 XiangShan `xs_core.py` emits a 20.4 MB, 381,918-line
  legacy IR file in 4.40 s, but current `pycc` rejects it at the required
  `pyc.frontend.contract` gate. It is therefore not a valid NewCircuit
  large-design performance or equivalence result.
- Disjoint and overlapping register slice splitting pass multi-cycle C++ and
  Icarus Verilog reset/enable/data tests. A named register remains intact for
  probe access.
- A 65-bit bitwise value and a 65-bit register each observed through 65
  one-bit readers split into 65 one-bit operations. The former passes a
  100-case C++ oracle; the latter passes multi-cycle C++ and Icarus Verilog
  reset/data checks. Coverage detection now sweeps slice boundaries instead
  of scanning every reader for every segment.
- Overlapping bitwise slice readers share a common four-bit segment for
  AND, OR, XOR and NOT. C++ and Icarus Verilog tests cover AND values and an
  X-state slice; a C++ test covers NOT values.
- Selective fixed-index vector register lane splitting passes C++ and Icarus
  Verilog reset, data and enable checks.
- Observed vector elementwise lanes and low-width truncated arithmetic pass
  C++ and Icarus Verilog tests, including arithmetic wraparound.
- Rank-two fixed-index vector register and elementwise reads lower to two
  observed scalar lanes and pass C++ plus Icarus Verilog checks.
- Reset-group runtime operations match individual scalar register tick/commit
  state over 100,000 deterministic randomized steps and vector register state
  over 30,000 steps, including calls that omit a commit; generated C++ uses the
  grouped clock/reset path.
- The bounded global initial partition joins independent adjacent pure DAG
  components while preserving effectful cuts. A group-order verifier rejects
  internal use-before-definition after coarsening, partitioning or replication.
- Packed activity flags skip a downstream group when an upstream group ran but
  its output stayed equal; a later real change activates it again. The plan
  verifies every packed input has an earlier pure-group producer. A 66-group
  generated design exercises the second 64-bit flag word against a scalar
  recurrence oracle.
- Packed propagation compares only the downstream group's demanded output
  bits; an observable full-width source can change elsewhere without waking
  its one-bit consumer. A 130-bit source tests masks crossing 64-bit words.
- A constant AND/OR operand narrows exact graph demand before group activation;
  a low input-bit change masked by a constant skips evaluation, while a live
  bit change still updates the output.
- Exact demand crosses nested `pyc.comb` result/yield and argument/input
  bindings. A high-bit input change leaves an upstream group dormant when its
  only consumer extracts the low bit after the nested regions.
- Constant-amount dynamic left, logical-right and arithmetic-right shifts
  propagate the exact demanded input bits. A 16-bit group tracks only bits
  0, 1 and 15 for three output slices, skipping a change to bit 7 while
  preserving all three output values.
- A two-bit shift amount causes those same three shift kinds to demand only
  input bits 0–3 and 15; the amount remains a full-demand group input. A
  five-bit amount on 64-bit values demands bits 0–31 and 63, skipping changes
  to bit 40 and preserving all outputs over 256 inputs and shift amounts.
- Hardware MLIR pushes four-bit extractions through fixed left, logical-right
  and arithmetic-right shifts, forming narrow source slices, zero constants,
  concatenations and sign extensions as needed. Focused IR dumps remove the
  full-width shifts for data-bearing, partial-fill and all-fill cases. C++ and
  Icarus Verilog agree on samples, including unknown sign bits in Verilog.
  The Verilog emitter now renders a one-bit sign-extension source as a scalar.
- Two readers of each 128-bit modular add, subtract and multiply share a 40-bit
  operation, the width of the highest observed bit. A 256-case C++ oracle and
  Icarus Verilog check carry, borrow and multiplication behavior.
- Two readers of a 128-bit mux share one 40-bit selection. A 256-case C++
  oracle and Icarus Verilog check both selected arms.
- Two readers of each 128-bit fixed left, logical-right and arithmetic-right
  shift retain only the low 40 output bits. The right shifts become input
  extracts with zero or sign extension when fill is needed. Constant-amount
  dynamic shifts with multiple readers enter the same path, while the
  single-reader fixture still exercises graph demand analysis on dynamic
  opcodes. A 256-case C++
  oracle and Icarus Verilog check both positive and negative operands.
- A singleton scalar division receives the same input-change activation as a
  larger group, and the feature can be disabled by `--sim-group-activation=false`.
- A singleton scalar ADD likewise skips a second evaluation with unchanged
  inputs, while `--sim-group-activation=false` retains eager execution.
  The C++ preserve-ops mode also suppresses singleton activation groups.
- Nested `pyc.comb` regions emit correctly in C++ and Verilog from captured
  graph steps; effectful body operations fail the dialect verifier.
- Vector dimension broadcasts with both outer and inner insertion positions
  emit from graph-owned shape and dimension fields; C++ and Icarus Verilog
  match for both layouts.
- Fixed-index reads of both dimension-broadcast layouts select the original
  source vector or one scalar lane with a smaller broadcast. The IR no longer
  contains the original dimension broadcasts; 32 C++ inputs and Icarus
  Verilog agree.
- Assignment value IDs and assertion condition/message are captured in the
  graph. A false C++ assertion aborts with its captured message; a true
  assertion passes.
- Register construction reads graph-owned input/output IDs and types. A
  register Q passed through a `pyc.comb` alias keeps both its output-port and
  named-register probe registration after the plan resolves passthroughs.
- A three-stage CDC graph binding delays data for three rising edges and
  resets the pipeline; the generated C++ uses the captured stage count.
- A depth-two FIFO graph binding preserves ready/valid and data across push
  and pop; its generated C++ matches the prior MLIR-accessor emission.
- A depth-four dual-clock FIFO graph binding transfers one item through pointer
  synchronization and then pops it; its generated C++ matches the prior path.
- A named depth-four synchronous memory graph binding reads the old value on
  a read/write collision and reads the new value next cycle; generated C++
  matches the prior path.
- A dual-read memory preserves independent registered read outputs across a
  write collision. A byte memory updates its asynchronous read after commit.
  Both graph-lowered C++ outputs match the prior path exactly.
- Hierarchical instance callee and port bindings now lower through SimGraph and
  SimulationPlan. The SCC state-feedback gate exercises instance evaluation,
  caching and two-phase tick with both normal and fast scheduling paths.
  Capturing declaration order, types and planned instance tick chunks leaves
  the focused generated C++ byte-for-byte identical.
- Six independent scalar muxes with one selector emit a single branch in a
  planned group. A forced three-operation codegen chunk falls back to six
  individual graph muxes; both forms produce the same outputs.
- Six muxes separated by independent bitwise operations are reordered within
  the pure DAG group and emit one selector branch; all twelve outputs match a
  64-case C++ oracle.
- The same six muxes form a shared selector group before a six-node bounded
  partition; both default and bounded variants emit one branch and pass the
  same 64-case oracle.
- SimulationPlan now lowers each pure group into verified expression or
  shared-condition statements and fixes codegen chunk boundaries. The C++
  emitter renders these statements without recomputing the group partition.
  It also renders planned comb-region step chunks.
- Scalar ADD replication with two consumer groups matches an arithmetic oracle
  over 10,000 inputs with replication enabled and disabled.
- One-use replication and duplicate uses in one consumer group remove the
  original execution node; enabled and disabled C++ variants match a 256-case
  arithmetic oracle for each shape.
- A cheap NOT expression with two add/sub readers in one pure group is inlined
  into both consumers. Enabled and disabled C++ variants match a 256-case
  arithmetic oracle; only the disabled variant materializes the NOT result.
- Recursive scalar replication removes two successive source nodes and emits
  their nested expression at one consumer; enabled and disabled variants match
  a 256-case arithmetic oracle.
- A fixed-index read from a materialized two-lane vector is copied into its
  consumer group and its standalone assignment is removed. Enabled and
  disabled variants match a 256-case C++ oracle.
- A vector division with 65 fixed-index readers becomes 65 scalar divisions
  in the shared Hardware MLIR pass. The generated C++ matches 32 rounds of
  independent lane inputs. A 65-lane vector register likewise becomes 65
  scalar registers and matches reset and multi-cycle C++ checks. A separate
  256-case lane oracle covers compare, truncate, sign extend, extract,
  dynamic and arithmetic shifts, and signed divide through the same rewrite.
- A singleton `v_get` retained by a one-operation partition compares only its
  selected lane. Changing an unrelated vector lane skips both the read and
  downstream add; changing the selected lane re-evaluates both.
- A grouped rank-two vector reduction propagates activity to a fixed-index
  row reader only when that row changes. The generated propagation condition
  reads row 0, and a three-step C++ oracle covers irrelevant and relevant
  input changes, with seven group evaluations and two cache skips.

These checks establish only the implemented subset, not full GSIM equivalence.

References: [GSIM source](https://github.com/OpenXiangShan/gsim),
[GSIM paper](https://talks-pubs.xiangshan.cc/publications/dac2025-GSIM.pdf).
