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
    return failure(); // unscheduled comb region
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
      llvm::errs() << "[comb-partition] dag build failed on @" << func.getSymName() << "\n";
      signalPassFailure();
      return;
    }
    auto plan = buildCombPartition(*dag, targetSize, func);
    if (failed(plan)) {
      llvm::errs() << "[comb-partition] plan failed on @" << func.getSymName() << "\n";
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
buildAtomPartitioning(unsigned atomCount, ArrayRef<uint64_t> atomWeight,
                      ArrayRef<SmallVector<unsigned>> atomPreds,
                      ArrayRef<unsigned> atomSlot, uint64_t targetSize);

FailureOr<CombPartitionPlan>
buildCombPartition(const ChangeScheduleDag &dag, uint64_t targetSize,
                   mlir::func::FuncOp func) {
  auto graph = buildRegionGraph(dag);
  if (failed(graph)) {
    func.emitError("comb partition: unscheduled comb region in slot order");
    return failure();
  }
  const RegionGraph &g = *graph;
  const unsigned regionCount = g.regions.size();

  CombPartitionPlan plan;
  plan.partitionCount = regionCount;
  if (regionCount == 0)
    return plan;

  // Collapse region-level strongly connected components into atoms. The
  // candidate DAG is acyclic, but condensing candidates into multi-result
  // regions can create cycles at region granularity (result A of region R
  // feeds region Q, whose result feeds result B of R). Members of one SCC
  // are mutually dependent and must be evaluated together, so they are a
  // single partitioning atom.
  llvm::SmallVector<unsigned> atomOfRegion(regionCount, 0u);
  {
    // Iterative Tarjan over the region graph.
    enum : unsigned char { kUnvisited = 0, kOpen = 1, kDone = 2 };
    llvm::SmallVector<unsigned char> state(regionCount, kUnvisited);
    llvm::SmallVector<unsigned> index(regionCount, 0u), low(regionCount, 0u);
    llvm::SmallVector<int> onStack(regionCount, 0);
    llvm::SmallVector<unsigned> stack;
    llvm::SmallVector<int64_t> iter(regionCount, -1);
    llvm::SmallVector<unsigned> frames;
    unsigned nextIndex = 0;
    unsigned atomCount = 0;
    for (unsigned root = 0; root < regionCount; ++root) {
      if (state[root] != kUnvisited)
        continue;
      frames.push_back(root);
      state[root] = kOpen;
      index[root] = low[root] = nextIndex++;
      stack.push_back(root);
      onStack[root] = 1;
      while (!frames.empty()) {
        unsigned v = frames.back();
        if (++iter[v] < static_cast<int64_t>(g.preds[v].size())) {
          // preds are predecessors; SCC over predecessors == over successors
          // for condensation purposes (same components).
          unsigned w = g.preds[v][iter[v]];
          if (state[w] == kUnvisited) {
            state[w] = kOpen;
            index[w] = low[w] = nextIndex++;
            stack.push_back(w);
            onStack[w] = 1;
            frames.push_back(w);
          } else if (onStack[w]) {
            low[v] = std::min(low[v], index[w]);
          }
          continue;
        }
        frames.pop_back();
        if (low[v] == index[v]) {
          unsigned w;
          do {
            w = stack.back();
            stack.pop_back();
            onStack[w] = 0;
            atomOfRegion[w] = atomCount;
          } while (w != v);
          ++atomCount;
        }
        if (!frames.empty()) {
          unsigned parent = frames.back();
          low[parent] = std::min(low[parent], low[v]);
        }
        state[v] = kDone;
      }
    }

    // Atom-level graph in slot-first order.
    const unsigned atomTotal = atomCount;
    llvm::SmallVector<uint64_t> atomWeight(atomTotal, 0u);
    for (unsigned r = 0; r < regionCount; ++r) {
      atomOfRegion[r] = atomTotal - 1 - atomOfRegion[r]; // reverse Tarjan ids
      atomWeight[atomOfRegion[r]] += g.weight[r];
    }
    llvm::SmallVector<llvm::DenseSet<unsigned>> atomPredSets(atomTotal);
    for (unsigned r = 0; r < regionCount; ++r)
      for (unsigned p : g.preds[r])
        if (atomOfRegion[p] != atomOfRegion[r])
          atomPredSets[atomOfRegion[r]].insert(atomOfRegion[p]);
    llvm::SmallVector<llvm::SmallVector<unsigned>> atomPreds(atomTotal);
    llvm::SmallVector<unsigned> atomSlot(atomTotal, 0u);
    llvm::SmallVector<bool> atomSeen(atomTotal, false);
    unsigned seenCount = 0;
    for (unsigned r : g.slotOrder) {
      unsigned a = atomOfRegion[r];
      if (!atomSeen[a]) {
        atomSeen[a] = true;
        atomSlot[a] = seenCount++;
      }
      for (unsigned p : atomPredSets[a])
        atomPreds[a].push_back(p);
    }
    for (unsigned a = 0; a < atomTotal; ++a)
      llvm::sort(atomPreds[a]);

    // Combine over atoms (same predecessor-merge rule as before).
    CombPartitionPlan atomPlan;
    auto atoms = buildAtomPartitioning(atomTotal, atomWeight, atomPreds,
                                       atomSlot, targetSize);
    if (failed(atoms)) {
      llvm::errs() << "[comb-partition] atom partitioning failed (cycle or size gate)\n";
      return failure();
    }

    plan.partitionCount = atoms->partitionCount;
    plan.partitions.assign(dag.operations.size(), 0u);
    for (unsigned t = 0; t < dag.operations.size(); ++t) {
      unsigned r = g.candidateToRegion[t];
      if (r == kNoRegion)
        continue;
      plan.partitions[t] = atoms->partitions[atomOfRegion[r]];
    }
    return plan;
  }
}

FailureOr<CombPartitionPlan>
buildAtomPartitioning(unsigned atomCount, ArrayRef<uint64_t> atomWeight,
                      ArrayRef<SmallVector<unsigned>> atomPreds,
                      ArrayRef<unsigned> atomSlot, uint64_t targetSize) {
  CombPartitionPlan plan;
  plan.partitionCount = atomCount;
  if (atomCount == 0)
    return plan;

  llvm::SmallVector<int64_t> partOf(atomCount, -1);
  llvm::SmallVector<uint64_t> partSize;
  llvm::SmallVector<unsigned> partAtoms;
  partSize.reserve(atomCount);
  partAtoms.reserve(atomCount);

  // Visit atoms in slot-first order; merge into the strongest-connected
  // predecessor partition when the size bound allows.
  llvm::SmallVector<unsigned> order(atomCount);
  for (unsigned a = 0; a < atomCount; ++a)
    order[atomSlot[a]] = a;
  for (unsigned a : order) {
    llvm::DenseMap<int64_t, unsigned> edgeCount;
    for (unsigned p : atomPreds[a]) {
      int64_t pp = partOf[p];
      if (pp >= 0)
        ++edgeCount[pp];
    }
    int64_t best = -1;
    unsigned bestCount = 0;
    for (unsigned p : atomPreds[a]) {
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
        partSize[best] + atomWeight[a] <= targetSize) {
      partOf[a] = best;
      partSize[best] += atomWeight[a];
      ++partAtoms[best];
    } else {
      // An atom heavier than targetSize stays its own partition: targetSize
      // bounds merging, it never forces an atom to split.
      partOf[a] = static_cast<int64_t>(partSize.size());
      partSize.push_back(atomWeight[a]);
      partAtoms.push_back(1);
    }
  }

  // The merge above can create cycles at partition granularity: an atom may
  // join one predecessor's partition while its other predecessors' partitions
  // gain edges into that same partition. Collapse strongly connected
  // partition groups until the partition graph is acyclic. Correctness forces
  // these unions, so the size bound is not enforced on them.
  bool forcedUnion = false;
  while (true) {
    unsigned partCount = partSize.size();
    llvm::SmallVector<llvm::DenseSet<int64_t>> partSucc(partCount);
    for (unsigned a = 0; a < atomCount; ++a) {
      for (unsigned p : atomPreds[a]) {
        int64_t from = partOf[p];
        int64_t to = partOf[a];
        if (from == to || from < 0 || to < 0)
          continue;
        partSucc[from].insert(to);
      }
    }
    // Kahn's algorithm to test acyclicity.
    llvm::SmallVector<unsigned> partIndegree(partCount, 0u);
    for (unsigned from = 0; from < partCount; ++from)
      for (int64_t to : partSucc[from])
        ++partIndegree[to];
    llvm::SmallVector<int64_t> ready;
    for (int64_t p = 0; p < static_cast<int64_t>(partCount); ++p)
      if (partIndegree[p] == 0)
        ready.push_back(p);
    unsigned visited = 0;
    while (!ready.empty()) {
      int64_t p = ready.pop_back_val();
      ++visited;
      for (int64_t succ : partSucc[p])
        if (--partIndegree[succ] == 0)
          ready.push_back(succ);
    }
    if (visited == partCount)
      break;

    // Find SCCs with more than one member (iterative Tarjan) and union each
    // into its lowest member id.
    enum : unsigned char { kUnvisited = 0, kOpen = 1, kDone = 2 };
    llvm::SmallVector<unsigned char> state(partCount, kUnvisited);
    llvm::SmallVector<unsigned> index(partCount, 0u), low(partCount, 0u);
    llvm::SmallVector<int> onStack(partCount, 0);
    llvm::SmallVector<int64_t> tarjanStack;
    llvm::SmallVector<int64_t> frames;
    llvm::SmallVector<unsigned> frameNext(partCount, 0u);
    unsigned nextIndex = 0;
    bool unioned = false;
    for (int64_t root = 0; root < static_cast<int64_t>(partCount); ++root) {
      if (state[root] != kUnvisited)
        continue;
      frames.push_back(root);
      state[root] = kOpen;
      index[root] = low[root] = nextIndex++;
      tarjanStack.push_back(root);
      onStack[root] = 1;
      while (!frames.empty()) {
        int64_t v = frames.back();
        if (frameNext[v] < partSucc[v].size()) {
          auto it = partSucc[v].begin();
          std::advance(it, frameNext[v]++);
          int64_t w = *it;
          if (state[w] == kUnvisited) {
            state[w] = kOpen;
            index[w] = low[w] = nextIndex++;
            tarjanStack.push_back(w);
            onStack[w] = 1;
            frames.push_back(w);
          } else if (onStack[w]) {
            low[v] = std::min(low[v], index[w]);
          }
          continue;
        }
        frames.pop_back();
        if (low[v] == index[v]) {
          llvm::SmallVector<int64_t> members;
          int64_t w;
          do {
            w = tarjanStack.back();
            tarjanStack.pop_back();
            onStack[w] = 0;
            members.push_back(w);
          } while (w != v);
          if (members.size() > 1) {
            forcedUnion = true;
            unioned = true;
            int64_t keep = *std::min_element(members.begin(), members.end());
            uint64_t mergedSize = 0;
            unsigned mergedAtoms = 0;
            for (int64_t m : members) {
              mergedSize += partSize[m];
              mergedAtoms += partAtoms[m];
            }
            for (unsigned a = 0; a < atomCount; ++a) {
              bool inSCC = false;
              for (int64_t m : members)
                if (partOf[a] == m) {
                  inSCC = true;
                  break;
                }
              if (inSCC)
                partOf[a] = keep;
            }
            partSize[keep] = mergedSize;
            partAtoms[keep] = mergedAtoms;
            for (int64_t m : members)
              if (m != keep) {
                partSize[m] = 0;
                partAtoms[m] = 0;
              }
          }
        }
        if (!frames.empty()) {
          int64_t parent = frames.back();
          low[parent] = std::min(low[parent], low[v]);
        }
        state[v] = kDone;
      }
    }
    if (!unioned)
      return failure(); // no progress; give up safely

    // Compact partition ids.
    llvm::SmallVector<int64_t> remap(partSize.size(), -1);
    llvm::SmallVector<uint64_t> newSize;
    llvm::SmallVector<unsigned> newAtoms;
    for (unsigned a = 0; a < atomCount; ++a) {
      int64_t old = partOf[a];
      if (remap[old] < 0) {
        remap[old] = static_cast<int64_t>(newSize.size());
        newSize.push_back(partSize[old]);
        newAtoms.push_back(partAtoms[old]);
      }
      partOf[a] = remap[old];
    }
    partSize = std::move(newSize);
    partAtoms = std::move(newAtoms);
  }
  if (targetSize > 0 && !forcedUnion) {
    for (auto [size, atoms] : llvm::zip(partSize, partAtoms)) {
      // Oversized single atoms are legal; only merged partitions must
      // respect the coarseness bound. Forced unions (cycle collapse) are
      // correctness-required and exempt.
      if (atoms > 1 && size > targetSize)
        return failure();
    }
  }

  plan.partitionCount = partSize.size();
  plan.partitions.assign(atomCount, 0u);
  for (unsigned a = 0; a < atomCount; ++a)
    plan.partitions[a] = static_cast<uint64_t>(partOf[a]);
  return plan;
}

std::unique_ptr<Pass> createCombPartitionPass(uint64_t targetSize) {
  return std::make_unique<CombPartitionPass>(targetSize);
}

} // namespace pyc
