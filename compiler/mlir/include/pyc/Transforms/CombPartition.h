#pragma once

#include <cstdint>

#include "mlir/Dialect/Func/IR/FuncOps.h"
#include "mlir/Pass/Pass.h"
#include "llvm/ADT/SmallVector.h"
#include "pyc/Transforms/ChangeDrivenSchedule.h"

namespace pyc {

/// Per-operation partition assignment attached next to the schedule metadata.
inline constexpr llvm::StringLiteral kChangeSchedulePartitionAttr =
    "pyc.change_schedule.partition";
/// Key appended to the pyc.change_schedule.summary dictionary.
inline constexpr llvm::StringLiteral kChangeSchedulePartitionCountKey =
    "partition_count";

struct CombPartitionPlan {
  /// Partition id per textual candidate index. All candidates of one operation
  /// share the same partition id.
  llvm::SmallVector<uint64_t> partitions;
  uint64_t partitionCount = 0;
};

/// ESSENT-style acyclic coarsening over the change-driven schedule DAG.
///
/// Nodes are fused pyc.comb regions. Walking the DAG in slot (topological)
/// order, each region joins the partition of its most strongly connected comb
/// predecessor whenever the combined size stays within `targetSize`; otherwise
/// it starts a new partition. Because a region only ever merges into a
/// predecessor partition, the partition-induced graph stays acyclic by
/// construction.
///
/// Only pyc.comb operations are partitioned; state sources and instances stay
/// standalone scheduling units. `targetSize == 0` disables merging and yields
/// identity partitions.
mlir::FailureOr<CombPartitionPlan>
buildCombPartition(const ChangeScheduleDag &dag, uint64_t targetSize);

std::unique_ptr<mlir::Pass> createCombPartitionPass(uint64_t targetSize);

} // namespace pyc
