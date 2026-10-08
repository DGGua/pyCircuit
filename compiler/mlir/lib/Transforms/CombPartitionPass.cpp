#include "pyc/Transforms/CombPartition.h"

#include "pyc/Dialect/PYC/PYCOps.h"

#include "mlir/IR/BuiltinAttributes.h"
#include "mlir/IR/BuiltinOps.h"
#include "mlir/Pass/Pass.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallVector.h"

using namespace mlir;

namespace pyc {
namespace {

inline constexpr unsigned kNoRegion = ~0u;

/// Comb-region-level DAG derived from the candidate DAG. Weight is the number
/// of scheduled results (candidates) of the region.
struct RegionGraph {
  llvm::SmallVector<Operation *> regions; // textual order
  llvm::SmallVector<uint64_t> weight;
  llvm::SmallVector<llvm::SmallVector<unsigned>> preds; // sorted, comb-only
  llvm::SmallVector<unsigned> slotOrder;                // slot-first order
  llvm::SmallVector<unsigned> candidateToRegion;        // kNoRegion for non-comb
};

FailureOr<RegionGraph> buildRegionGraph(const ChangeScheduleDag &dag) {
  RegionGraph g;
  g.candidateToRegion.assign(dag.operations.size(), kNoRegion);
  llvm::DenseMap<Operation *, unsigned> regionIndex;
  for (auto indexed : llvm::enumerate(dag.operations)) {
    Operation *op = indexed.value();
    if (!isa<CombOp>(op))
      continue;
    auto [entry, inserted] = regionIndex.try_emplace(op, g.regions.size());
    if (inserted) {
      g.regions.push_back(op);
      g.weight.push_back(0);
    }
    g.candidateToRegion[indexed.index()] = entry->second;
    ++g.weight[entry->second];
  }

  llvm::SmallVector<llvm::DenseSet<unsigned>> predSets(g.regions.size());
  for (unsigned t = 0; t < dag.operations.size(); ++t) {
    unsigned to = g.candidateToRegion[t];
    if (to == kNoRegion)
      continue;
    for (unsigned p : dag.predecessors[t]) {
      unsigned po = g.candidateToRegion[p];
      if (po != kNoRegion && po != to)
        predSets[to].insert(po);
    }
  }
  g.preds.resize(g.regions.size());
  for (unsigned r = 0; r < g.regions.size(); ++r) {
    g.preds[r].assign(predSets[r].begin(), predSets[r].end());
    llvm::sort(g.preds[r]);
  }

  llvm::SmallVector<bool> seen(g.regions.size(), false);
  for (unsigned t : dag.scheduleToTextual) {
    unsigned r = g.candidateToRegion[t];
    if (r != kNoRegion && !seen[r]) {
      seen[r] = true;
      g.slotOrder.push_back(r);
    }
  }
  if (g.slotOrder.size() != g.regions.size())
    return failure();
  return std::move(g);
}

struct CombPartitionPass
    : public PassWrapper<CombPartitionPass, OperationPass<func::FuncOp>> {
  MLIR_DEFINE_EXPLICIT_INTERNAL_INLINE_TYPE_ID(CombPartitionPass)

  CombPartitionPass() = default;
  explicit CombPartitionPass(uint64_t targetSize) : targetSize(targetSize) {}

  StringRef getArgument() const override { return "pyc-comb-partition"; }
  StringRef getDescription() const override {
    return "Attach ESSENT-style acyclic comb partition metadata to the "
           "change-driven schedule";
  }

  void runOnOperation() override {
    func::FuncOp func = getOperation();
    if (func.isDeclaration())
      return;

    auto summary =
        func->getAttrOfType<DictionaryAttr>(kChangeScheduleSummaryAttr);
    if (!summary) {
      func.emitError("comb partitioning requires pyc.change_schedule.summary; "
                     "run pyc-change-driven-schedule first");
      signalPassFailure();
      return;
    }

    // Idempotency: drop previous partition assignments before re-planning.
    func.walk([](Operation *op) { op->removeAttr(kChangeSchedulePartitionAttr); });
    llvm::SmallVector<NamedAttribute> summaryItems;
    for (NamedAttribute item : summary) {
      if (item.getName() == kChangeSchedulePartitionCountKey)
        continue;
      summaryItems.push_back(item);
    }

    auto dag = buildChangeScheduleDag(func);
    if (failed(dag)) {
      signalPassFailure();
      return;
    }
    auto plan = buildCombPartition(*dag, targetSize);
    if (failed(plan)) {
      func.emitError("comb partition planning failed");
      signalPassFailure();
      return;
    }

    Builder builder(func.getContext());
    llvm::DenseMap<Operation *, uint64_t> regionPartition;
    for (auto indexed : llvm::enumerate(dag->operations)) {
      if (!isa<CombOp>(indexed.value()))
        continue;
      regionPartition[indexed.value()] = plan->partitions[indexed.index()];
    }
    for (auto &entry : regionPartition)
      entry.first->setAttr(kChangeSchedulePartitionAttr,
                           builder.getI64IntegerAttr(
                               static_cast<int64_t>(entry.second)));

    summaryItems.push_back({builder.getStringAttr(kChangeSchedulePartitionCountKey),
                            builder.getI64IntegerAttr(
                                static_cast<int64_t>(plan->partitionCount))});
    llvm::sort(summaryItems, [](const NamedAttribute &lhs,
                                const NamedAttribute &rhs) {
      return lhs.getName().str() < rhs.getName().str();
    });
    func->setAttr(kChangeScheduleSummaryAttr,
                  builder.getDictionaryAttr(summaryItems));
  }

  uint64_t targetSize = 0;
};

} // namespace

FailureOr<CombPartitionPlan>
buildCombPartition(const ChangeScheduleDag &dag, uint64_t targetSize) {
  auto graph = buildRegionGraph(dag);
  if (failed(graph))
    return failure();
  const RegionGraph &g = *graph;
  const unsigned regionCount = g.regions.size();

  CombPartitionPlan plan;
  plan.partitionCount = regionCount;
  if (regionCount == 0)
    return plan;

  llvm::SmallVector<int64_t> partOf(regionCount, -1);
  llvm::SmallVector<uint64_t> partSize;
  llvm::SmallVector<unsigned> partRegions;
  partSize.reserve(regionCount);
  partRegions.reserve(regionCount);

  for (unsigned r : g.slotOrder) {
    // Count comb dependency edges from this region into each predecessor
    // partition; the strongest connection (ties broken by lowest partition id)
    // is the merge candidate.
    llvm::DenseMap<int64_t, unsigned> edgeCount;
    for (unsigned p : g.preds[r]) {
      int64_t pp = partOf[p];
      if (pp >= 0)
        ++edgeCount[pp];
    }
    int64_t best = -1;
    unsigned bestCount = 0;
    for (unsigned p : g.preds[r]) {
      int64_t pp = partOf[p];
      if (pp < 0)
        continue;
      unsigned c = edgeCount.lookup(pp);
      if (c > bestCount || (c == bestCount && pp < best)) {
        best = pp;
        bestCount = c;
      }
    }

    if (targetSize > 0 && best >= 0 &&
        partSize[best] + g.weight[r] <= targetSize) {
      partOf[r] = best;
      partSize[best] += g.weight[r];
      ++partRegions[best];
    } else {
      // A single region larger than targetSize is kept as its own partition:
      // targetSize bounds merging, it never forces a region to split.
      partOf[r] = static_cast<int64_t>(partSize.size());
      partSize.push_back(g.weight[r]);
      partRegions.push_back(1);
    }
  }

  // Gate: the partition-induced graph must be acyclic. Because merges only
  // target predecessor partitions and regions are visited in slot order, every
  // cross-partition edge points forward in slot order; verify that directly.
  llvm::SmallVector<unsigned> slotOf(regionCount, 0u);
  for (auto indexed : llvm::enumerate(g.slotOrder))
    slotOf[indexed.value()] = indexed.index();
  for (unsigned r = 0; r < regionCount; ++r) {
    for (unsigned p : g.preds[r]) {
      if (partOf[p] == partOf[r])
        continue;
      if (slotOf[p] >= slotOf[r])
        return failure();
    }
  }
  if (targetSize > 0) {
    for (auto [size, regions] : llvm::zip(partSize, partRegions)) {
      // Oversized singletons are legal (see above); only merged partitions
      // must respect the coarseness bound.
      if (regions > 1 && size > targetSize)
        return failure();
    }
  }

  plan.partitionCount = partSize.size();
  plan.partitions.assign(dag.operations.size(), 0u);
  for (unsigned t = 0; t < dag.operations.size(); ++t) {
    unsigned r = g.candidateToRegion[t];
    if (r == kNoRegion)
      continue;
    plan.partitions[t] = static_cast<uint64_t>(partOf[r]);
  }
  return plan;
}

std::unique_ptr<Pass> createCombPartitionPass(uint64_t targetSize) {
  return std::make_unique<CombPartitionPass>(targetSize);
}

} // namespace pyc
