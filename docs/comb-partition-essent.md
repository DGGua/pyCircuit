# Comb Partitioning (ESSENT-style combine)

Branch: `ldz/feat/combine-split` (derived from `feat/change-driven-scheduler`)

This document describes the acyclic comb-domain partitioning introduced on this
branch. The design follows the coarsening idea of ESSENT
(Beamer et al., *ESSENT: A High-Performance RTL Simulator*, WoSET 2021):
instead of paying per-node activity checks, coarsen the graph into partitions
and skip whole partitions when their inputs are unchanged.

## Relation to the removed `PartitionCombPass`

`PartitionCombPass` (supernode static partitioning, removed in `c7e5744`) was a
general supernode partitioner with a large contract surface (~2.3k lines with
its checkers). This branch deliberately re-implements only the essential
kernel on top of the existing change-driven schedule infrastructure:

| Reused infrastructure | Role |
| --- | --- |
| `CombDepGraph` / `buildChangeScheduleDag` | same-tick candidate dependency DAG |
| `pyc.change_schedule.v1` metadata | slot/rank/fanout contract, extended additively |
| `CppEmitter` comb path | input-snapshot guards, result publish filtering |
| `pyc::cpp::DirtyBitset` | runtime dirty set, now keyed by partition |

## Algorithm (combine)

Atoms are the fused `pyc.comb` regions produced by `pyc-fuse-comb`. The
partitioner (`pyc-comb-partition`, `CombPartitionPass.cpp`) walks regions in
schedule slot (topological) order:

1. Count comb dependency edges from region `r` into each predecessor
   partition.
2. Pick the strongest-connected predecessor partition (ties: lowest id).
3. If the merged weight (number of scheduled results) stays within
   `targetSize`, join it; otherwise start a new partition.

Because a region only ever merges into a **predecessor** partition, the
partition-induced graph is acyclic by construction. The pass still verifies
this directly (every cross-partition edge must point forward in slot order) as
a gate, together with the size bound.

State sources (regs, memories, fifos, CDC syncs) and instances are never
merged; they remain standalone schedule units and act as partition boundaries.

The result is attached as:
- `pyc.change_schedule.partition = <id>` per comb operation,
- `partition_count` appended to `pyc.change_schedule.summary`.

## Emission

The C++ emitter groups combs by partition and emits one
`eval_comb_part_<id>()` method per partition:

- a single guard takes the partition dirty bit and compares the polled
  boundary-input snapshots of **all** member regions once,
- member bodies then run unconditionally in schedule order; results are still
  published through the unchanged change-compare + fanout path, so
  intra-partition consumers need no re-marking (marks targeting the partition
  currently being evaluated are suppressed),
- dispatch sites (`eval_comb_pass`, dirty tick) call the group method only at
  the first member's position; partitions without any polled boundary input are
  called only when their dirty bit is set.

Without partition metadata the emitter falls back to **identity partitions**
(one per scheduled operation), which reproduces the pre-partition dispatch
exactly — the default pipeline is unchanged bit-for-bit.

## Usage

```bash
pycc design.py --emit=cpp --comb-partition-size=64   # coarseness target
pycc design.py --emit=cpp                            # partitioning off
```

`--comb-partition-size=0` (default) disables the pass. As in ESSENT, a single
coarseness parameter controls the tradeoff between skip granularity and
per-partition check overhead; results are independent of the value chosen
(only performance changes).

## Verification

- `compiler/mlir/test/comb_partition_smoke.sh`: compiles the dirty-scheduler
  design in both modes and diffs 256-cycle simulation traces bit-for-bit; also
  checks the partitioned eval structure and the IR metadata contract.
- `compiler/mlir/test/change_schedule_gate.sh`: unchanged schedule contract.
- `compiler/mlir/test/comb_dirty_scheduler_smoke.sh` and
  `cpp_member_placement_smoke.sh`: identity-mode emission unchanged.

### Future work (split phase)

ESSENT additionally exploits dynamic don't-cares (e.g. unselected mux ways and
write-disabled register inputs) to shrink the evaluated portion further. That
"split" direction is intentionally left out of this branch.
