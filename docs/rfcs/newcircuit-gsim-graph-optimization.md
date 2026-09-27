# NewCircuit: GSIM optimization ownership and coverage

The target is the enabled compiler pipeline in the local read-only `gsim/`
snapshot, adapted to PYC hardware semantics. The source-level decisions,
including differences from the paper and disabled source functions, are in
[newcircuit-gsim-source-audit.md](newcircuit-gsim-source-audit.md).

The implementation includes Hardware MLIR value/state transformations,
SimGraph coarsening, partitioning and replication, and verified SimulationPlan
activity, reset and statement scheduling. The acceptance corpus checks the
intended IR/graph structure as well as values. It is not a proof of equivalence
for every legal circuit or a reproduction of the paper's large-core speedups.

## Ownership and current coverage

Hardware changes belong to dialect operations and shared MLIR passes before
both C++ and Verilog. Execution-only decisions belong to SimGraph and
SimulationPlan. The emitter renders the verified plan. Decisions
0112/0114/0115/0116/0121/0122/0127/0128/0129/0134/0135 remain binding.

| Enabled GSIM mechanism | NewCircuit implementation and boundaries | Acceptance gate |
|---|---|---|
| Width inference, legality, dead nodes, aliases, constants, expression patterns | Typed PYC IR; existing cycle/depth/domain gates, SCCP, canonicalization, CSE and state cleanup. Concat/shifted-OR equality, one-hot extraction and partial cast/shift readers run in shared Hardware MLIR. | `simulation_plan`, `semantic` |
| `splitArray` and aggregate demand | Recursive fixed-index vector state/expression lanes, dimension broadcasts and selected rank-two reductions. Named lane aliases keep distinct probes. Observable aggregate values remain whole through SLP, register packing and optional vector unrolling. Source optional splitting rejects variable indices. | `array_split`, `simulation_plan` |
| `usedBits`, value-level `splitNodes` | Shared scalar bitwise/mux segments, modular arithmetic narrowing, fixed shifts and casts, single-reader and overlapping register slices; truncation propagates through bitwise, cast, mux/select and fixed-shift chains to the greedy fixed point; pure comb result/input boundaries carry partial demand without removing the regions. Dynamic left shifts preserve low-prefix demand. Width/keep roots remain barriers. Memory storage remains full-width because every legal PYC memory exposes hash/watch/dump. | `scalar_demand`, `scalar_fixedpoint`, `array_split`, `simulation_plan` |
| Observable roots | Explicit `debug_keep` values acquire retained observation aliases before generic cleanup; a dedicated non-hardware resource prevents DCE/CSE from dropping them. Nested comb effects retain the root without creating state or a combinational cut. Name-only unused values remain hints under the existing contract. | `array_split`, `simulation_plan` |
| `commonExpr` | Shared MLIR SSA values plus CSE cover existing whole-node expression equivalence. Source selective whole-node CSE is distinct from arbitrary subtree extraction. | `simulation_plan`, `replication` |
| Correlation coarsening | One reverse out-degree-one pass, one forward in-degree-one pass, then sibling merging; immediate quotient updates, cycle checks, exact recipient-size guards and collision-safe predecessor-set comparison. | `partition` |
| Initial partition | Default dynamic programming operates on indivisible coarse units, distinct quotient edges and the first minimum-cost tie. `--sim-supernode-max-size` is a soft target: an oversized coarse unit remains whole. `--sim-supernode-strict-bound=true` retains the earlier experimental hard-bound policy. Zero disables partitioning. | `partition` |
| Shared conditions / statement trees | Dependency-ready pure scheduling collects at least six scalar mux/select nodes sharing a selector, including late-ready operands and inlined branch trees. Verifiers reject interdependent batches and crossing source effect boundaries. PYC SSA muxes replace FIRRTL last-connect `when`. | `shared_condition` |
| Cost-driven inline / `replicationOpt` | Cost-bounded scalar expression trees inline within groups. Singleton supernodes can replicate compound trees using recursive host-operation cost times distinct consumer groups `<3`; source execution and stale outputs are removed. Wide/aggregate results, observed roots and effectful boundaries stay materialized. | `replication`, `simulation_plan` |
| Exact demand and activation | Backward scalar masks cross nested comb argument/yield bindings and dynamic shifts; fixed vector lanes/elements receive selective checks. Group input caches and packed propagation use the same demand. | `simulation_plan`, `activity` |
| Packed propagation across state/effects | Complete producers publish changes in their actual eval/commit phases: registers, CDC, memories, FIFOs and instances as well as pure nodes. SCC/effect cases without a complete publication schedule keep conservative caches. Byte-memory and async-FIFO outputs can publish from both eval and commit. | `activity`, `primitive_activity`, `semantic` |
| Packed checks and activation cost | The plan chooses branchless writes for at most three bitmap words and one shared branch above that cost. Consecutive groups in one eight-bit block receive an outer quiet check; code chunks cannot silently break a planned batch. | `activity` |
| Reset slow path | Registers sharing controls use outlined reset helpers and one unlikely reset branch. Pending next state is applied at commit, retaining TICK/XFER and repeated-call behavior. No reset effect is moved into `comb()`. | `activity`, `simulation_plan`, `semantic` |
| Topological order, primitive/hierarchy execution and C++ emission | SimulationAST freezes expression semantics and effectful bindings; SimGraph owns node/value IDs; SimulationPlan verifies order, statements, activity, caches, chunks and phases. C++ renders graph expressions and planned actions. Existing hierarchy, SCC fallback and runtime primitives remain the PYC implementation. | `simulation_plan`, `primitive_activity`, `semantic` |

Wide dynamic shift amounts also normalize in shared MLIR before conversion to a
host `unsigned`, preserving overshift behavior and unknown amount propagation.
This prevents a 64/128-bit amount such as `1 << 32` from becoming zero in C++.

Source `graphRefine`, `mergeRegister` and `constructRegs` are disabled and are
not implementation requirements. FIRRTL width/clock inference and invalid-value
rules cannot replace PYC's explicit types, enables, CDC and observation phases.
There is no legal unobserved anonymous PYC memory to narrow: PYC942 requires a
stable name and the runtime exposes full storage. The memory negative gate
checks this boundary, including high-byte writes and old-data behavior.

## Validation entry point

```sh
CCACHE_DISABLE=1 PYC_SIM_JOBS=2 \
  PYC_GATE_RUN_ID=newcircuit-gsim-20260926 \
  bash tests/newcircuit/run_gsim_reproduction_gate.sh
```

The umbrella runs `simulation_plan`, `partition`, `replication`,
`shared_condition`, `array_split`, `scalar_demand`, `scalar_fixedpoint`, `activity`,
`primitive_activity`, and the repository's semantic regressions. Dedicated
checks require both structural evidence and an independent value oracle;
execution rewrites compare enabled/disabled variants. Hardware rewrites are
checked in C++ and Verilog, including reset, observations and X/Z boundaries.

`benchmark_gsim_pipeline.py` measures compile time, emitted size and evaluation
throughput with an independently checked checksum. It is a synthetic 260-stage
DAG with infrequent input changes, not a GSIM/XiangShan benchmark. No large-core
or paper-level performance claim follows from it.

The local CMake configuration does not provide `MLIRRegisterAllPasses`, so the
optional `pyc-opt` target is excluded; build validation uses `pycc`.

The older evidence below records earlier stages of implementation. Its group
counts, policy descriptions and byte-identical comparisons describe those
revisions; the current gate explicitly requests strict bounds where a fixture
needs one-operation partitions. Default-policy behavior has a separate gate.

## Gate evidence (2026-09-26–27)

The final `pycc` build and all ten gate suites above passed. The complete main
suite and the remaining suites ran separately against the same compiler;
`docs/gates/logs/newcircuit-gsim-20260926/gsim_reproduction_summary.json`
records the compiler SHA256 and accepted logs. API hygiene, shell/Python syntax
and `git diff --check` also passed. No reference GSIM source was changed.

The added scalar fixed-point gate covers 19 shapes, each with 4,096 C++ and
Verilog cycles, including partial state, reset/enable, casts, mux/select and
nested comb interfaces. Full-width and retained observations have negative
structural checks. The scalar shift gate covers 64/128-bit amounts, overshifts
and X/Z amounts. Simulator diagnostics are checked independently of exit status.

The optional `run_gsim_reference_gate.sh` compares matching FIRRTL/PYC designs
using one independent oracle. GSIM and NewCircuit each passed 197,632 cases,
with digest `e4ccd8d72edf7475`. This used a non-official GCC 13.1 reference build
and a checked small-model fallback, because Clang >=19 was unavailable; details
are in `reference-toolchain.txt` and the source audit. It validates small
combinational arithmetic/mux/slice behavior, not large-core performance.

After the compile/gate jobs finished, the synthetic activity benchmark ran
10,000,000 evaluations three times per variant. Both variants matched the
independent checksum `327574114848`.

| Variant | Median elapsed | Evaluations/second | Emitted C++ bytes |
|---|---:|---:|---:|
| Default activity | 0.463253 s | 21.59 million | 28,423 |
| `--sim-group-activation=false` | 2.347100 s | 4.26 million | 27,391 |

The 5.07x ratio applies only to this 260-stage DAG with state changes every
64 evaluations. Emission took about 0.043/0.042 seconds and C++ compilation
1.85/1.69 seconds. All individual samples are in `benchmark.json`; this is
neither a comparison against GSIM throughput nor a reproduction of the paper's
large-core results.

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
