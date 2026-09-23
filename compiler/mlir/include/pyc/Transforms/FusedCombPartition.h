#pragma once

#include <memory>

#include "mlir/IR/Operation.h"
#include "mlir/Support/LogicalResult.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/SmallVector.h"

namespace pyc {

class CombOp;

/// Stable operation-level DAG for one validated, single-block pyc.comb body.
class FusedCombDepGraph {
public:
  static mlir::FailureOr<std::unique_ptr<FusedCombDepGraph>> build(CombOp comb);

  llvm::ArrayRef<mlir::Operation *> getOperations() const {
    return operations_;
  }
  llvm::ArrayRef<unsigned> getPredecessors(unsigned node) const {
    return predecessors_[node];
  }
  llvm::ArrayRef<unsigned> getSuccessors(unsigned node) const {
    return successors_[node];
  }

  /// Return a deterministic topological node order, using body order to break
  /// ties. Failure indicates a cycle.
  mlir::FailureOr<llvm::SmallVector<unsigned>> stableTopologicalOrder() const;

private:
  llvm::SmallVector<mlir::Operation *> operations_;
  llvm::SmallVector<llvm::SmallVector<unsigned>> predecessors_;
  llvm::SmallVector<llvm::SmallVector<unsigned>> successors_;
};

/// A contiguous partitioning of `order`. Each entry in `ends` is an exclusive
/// end offset; the final entry is `order.size()`.
struct FusedCombPartitionPlan {
  llvm::SmallVector<unsigned> order;
  llvm::SmallVector<unsigned> ends;
};

/// Plan placement-free local partitions, each containing at most maxNodes body
/// operations. The planner deterministically coarsens safe DAG neighborhoods
/// before minimizing cut edges over contiguous topological intervals.
mlir::FailureOr<FusedCombPartitionPlan>
planFusedCombPartitions(const FusedCombDepGraph &graph, unsigned maxNodes);

} // namespace pyc
