#include "pyc/Transforms/CombPartition.h"

#include "pyc/Dialect/PYC/PYCOps.h"

#include "mlir/IR/BuiltinAttributes.h"
#include "mlir/IR/BuiltinOps.h"
#include "mlir/Pass/Pass.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallSet.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallVector.h"

#include <functional>

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
buildCombPartition(const ChangeScheduleDag &dag, uint64_t targetSize,
                   mlir::func::FuncOp func) {
  // ESSENT-style acyclic partitioning (Beamer & Donofrio, DAC 2020,
  // Section IV): bootstrap with MFFC decomposition, then merge phases
  // (single-parent, small+small siblings, small+any sibling) guarded by the
  // external-path merge test.
  //
  // Our graph nodes are scheduled comb results (candidates) rather than raw
  // netlist gates; comb regions are the emission unit, so candidates of one
  // region are constrained into one partition.

  // 1. Candidate-level comb-only graph.
  const unsigned totalCandidates = dag.operations.size();
  llvm::SmallVector<unsigned> localOf(totalCandidates, ~0u);
  llvm::SmallVector<unsigned> globalOf;
  globalOf.reserve(totalCandidates);
  for (unsigned t = 0; t < totalCandidates; ++t) {
    if (!isa<CombOp>(dag.operations[t]))
      continue;
    localOf[t] = globalOf.size();
    globalOf.push_back(t);
  }
  const unsigned n = globalOf.size();
  CombPartitionPlan plan;
  plan.partitionCount = n;
  if (n == 0)
    return plan;

  llvm::SmallVector<llvm::SmallVector<unsigned>> succs(n), preds(n);
  for (unsigned t = 0; t < totalCandidates; ++t) {
    unsigned lt = localOf[t];
    if (lt == ~0u)
      continue;
    for (unsigned p : dag.predecessors[t]) {
      unsigned lp = localOf[p];
      if (lp != ~0u)
        preds[lt].push_back(lp);
    }
    for (unsigned sc : dag.successors[t]) {
      unsigned ls = localOf[sc];
      if (ls != ~0u)
        succs[lt].push_back(ls);
    }
  }

  // Union-find over partitions.
  unsigned partCount = n;
  llvm::SmallVector<int> parent(n);
  llvm::SmallVector<uint64_t> psize(n, 1);
  for (int i = 0; i < static_cast<int>(n); ++i)
    parent[i] = i;
  std::function<int(int)> find = [&](int x) {
    while (parent[x] != x) {
      parent[x] = parent[parent[x]];
      x = parent[x];
    }
    return x;
  };
  auto members = [&](int root) {
    llvm::SmallVector<unsigned> out;
    for (unsigned i = 0; i < n; ++i)
      if (find(i) == root)
        out.push_back(i);
    return out;
  };
  auto unionPartitions = [&](int a, int b) {
    int ra = find(a), rb = find(b);
    if (ra == rb)
      return ra;
    if (psize[ra] < psize[rb])
      std::swap(ra, rb);
    parent[rb] = ra;
    psize[ra] += psize[rb];
    --partCount;
    return ra;
  };

  // 2. MFFC bootstrap: crawl upward from each unassigned candidate (slot
  // order reversed for determinism); a predecessor joins the cone only when
  // all of its comb successors are already inside the cone.
  llvm::SmallVector<int> coneOf(n, -1);
  llvm::SmallVector<llvm::SmallVector<unsigned>> cones;
  llvm::SmallVector<unsigned> slotOrderLocal;
  slotOrderLocal.reserve(n);
  {
    llvm::SmallVector<bool> seen(n, false);
    for (unsigned t : dag.scheduleToTextual) {
      unsigned lt = localOf[t];
      if (lt != ~0u && !seen[lt]) {
        seen[lt] = true;
        slotOrderLocal.push_back(lt);
      }
    }
  }
  for (auto it = slotOrderLocal.rbegin(); it != slotOrderLocal.rend(); ++it) {
    unsigned v = *it;
    if (coneOf[v] >= 0)
      continue;
    int id = static_cast<int>(cones.size());
    cones.emplace_back();
    llvm::SmallVector<unsigned, 16> work{v};
    coneOf[v] = id;
    cones[id].push_back(v);
    while (!work.empty()) {
      unsigned u = work.pop_back_val();
      for (unsigned p : preds[u]) {
        if (coneOf[p] >= 0)
          continue;
        bool allInside = true;
        for (unsigned sc : succs[p])
          if (coneOf[sc] != id) {
            allInside = false;
            break;
          }
        if (!allInside)
          continue;
        coneOf[p] = id;
        cones[id].push_back(p);
        work.push_back(p);
      }
    }
  }

  // External-path merge test: partitions A and B may merge iff no external
  // path exists in either direction (a path through nodes outside A∪B).
  unsigned testEpoch = 0;
  llvm::SmallVector<unsigned> testMark(n, 0);
  auto externalPath = [&](int rootA, int rootB) {
    // Returns true if an external path exists from A to B (or B to A).
    auto direction = [&](int from, int to) {
      ++testEpoch;
      llvm::SmallVector<unsigned, 32> fromMembers = members(from);
      llvm::SmallVector<unsigned, 32> toMembers = members(to);
      for (unsigned m : fromMembers)
        testMark[m] = testEpoch;
      for (unsigned m : toMembers)
        testMark[m] = testEpoch;
      llvm::SmallVector<unsigned, 32> work;
      for (unsigned m : fromMembers)
        for (unsigned sc : succs[m])
          if (find(sc) != from && find(sc) != to)
            work.push_back(sc);
      while (!work.empty()) {
        unsigned u = work.pop_back_val();
        if (testMark[u] == testEpoch)
          continue;
        testMark[u] = testEpoch;
        if (find(u) == to)
          return true;
        for (unsigned sc : succs[u])
          if (find(sc) != from && find(sc) != to)
            work.push_back(sc);
      }
      return false;
    };
    return direction(rootA, rootB) || direction(rootB, rootA);
  };

  // Region constraint: candidates of one comb region must share a partition.
  {
    llvm::DenseMap<Operation *, int> regionFirst;
    for (unsigned t = 0; t < totalCandidates; ++t) {
      if (localOf[t] == ~0u)
        continue;
      Operation *op = dag.operations[t];
      auto [it, inserted] = regionFirst.try_emplace(op, find(localOf[t]));
      if (inserted)
        continue;
      int a = it->second, b = find(localOf[t]);
      if (a == b)
        continue;
      // Region constraint forces this merge even if the external-path test
      // fails; the final SCC collapse restores acyclicity.
      unionPartitions(a, b);
    }
  }

  // Partition input adjacency: inputsOf[Q] = set of partitions feeding Q.
  auto buildInputs = [&]() {
    llvm::DenseMap<int, llvm::DenseSet<int>> inputs;
    for (unsigned v = 0; v < n; ++v) {
      int rv = find(v);
      for (unsigned p : preds[v]) {
        int rp = find(p);
        if (rp != rv)
          inputs[rv].insert(rp);
      }
    }
    return inputs;
  };
  // Undirected adjacency (cut edges between partitions, both directions).
  auto buildAdjacency = [&]() {
    llvm::DenseMap<int, llvm::DenseMap<int, unsigned>> adj;
    for (unsigned v = 0; v < n; ++v) {
      int rv = find(v);
      for (unsigned p : preds[v]) {
        int rp = find(p);
        if (rp == rv)
          continue;
        ++adj[rv][rp];
        ++adj[rp][rv];
      }
    }
    return adj;
  };

  auto mergeWithTest = [&](int a, int b) {
    int ra = find(a), rb = find(b);
    if (ra == rb)
      return true;
    if (externalPath(std::min(ra, rb), std::max(ra, rb)))
      return false;
    unionPartitions(ra, rb);
    return true;
  };

  // Phase A: single-parent partitions merge into their (unique) parent.
  bool progressed = true;
  while (progressed && partCount > 1) {
    progressed = false;
    auto inputs = buildInputs();
    for (auto &[p, ins] : inputs) {
      if (ins.size() != 1 || find(p) != p)
        continue;
      int q = *ins.begin();
      if (find(q) != q)
        continue;
      unionPartitions(p, q); // always acyclic-safe (paper Figure 4A)
      progressed = true;
      if (partCount <= 1)
        break;
    }
  }

  // Phases B/C: repeatedly merge small partitions with siblings.
  auto isSmall = [&](int root) {
    return targetSize > 0 && psize[find(root)] < targetSize;
  };
  for (int phase = 0; phase < 2 && partCount > 1; ++phase) {
    bool anyMerge = true;
    while (anyMerge && partCount > 1) {
      anyMerge = false;
      auto adj = buildAdjacency();
      auto inputs = buildInputs();
      // Deterministic scan over small partitions.
      for (unsigned v = 0; v < n && partCount > 1; ++v) {
        int p = find(v);
        if (p != static_cast<int>(v) || !isSmall(p))
          continue;
        auto it = adj.find(p);
        if (it == adj.end())
          continue;
        int best = -1;
        uint64_t bestCut = 0;
        double bestCommon = -1.0;
        for (auto &[q, cut] : it->second) {
          int rq = find(q);
          if (rq == p)
            continue;
          if (phase == 0 && !isSmall(rq))
            continue;
          double common = 0.0;
          auto ip = inputs.find(p);
          auto iq = inputs.find(rq);
          if (ip != inputs.end() && iq != inputs.end()) {
            unsigned shared = 0;
            for (int x : ip->second)
              if (iq->second.contains(x) || rq == x)
                ++shared;
            unsigned united = ip->second.size() + iq->second.size() - shared;
            common = united > 0 ? static_cast<double>(shared) / united : 0.0;
          }
          bool better = false;
          if (phase == 0)
            better = cut > bestCut || (cut == bestCut && rq < best);
          else
            better = common > bestCommon ||
                     (common == bestCommon && cut > bestCut);
          if (better) {
            best = rq;
            bestCut = cut;
            bestCommon = common;
          }
        }
        if (best >= 0 && mergeWithTest(p, best)) {
          anyMerge = true;
          progressed = true;
          break; // restart scan; partition ids changed
        }
      }
    }
  }

  // Final guard: collapse any residual cycles (forced region unions can
  // introduce them). Correctness requires acyclicity for singular execution.
  plan.partitionCount = partCount;
  plan.partitions.assign(totalCandidates, 0u);
  {
    // Dense-id the union-find roots: root ids are cone ids (0..n-1), while
    // partCount is only the surviving count, so raw ids must not index
    // partition-sized arrays.
    llvm::DenseMap<int, int> denseRoot;
    llvm::SmallVector<int> denseOf(n, -1);
    for (unsigned v = 0; v < n; ++v) {
      auto [it, inserted] = denseRoot.try_emplace(find(v), denseRoot.size());
      denseOf[v] = it->second;
      (void)inserted;
    }
    // Build partition graph and collapse SCCs until acyclic. Dense ids are
    // rebuilt every iteration because unions change the root set.
    bool acyclic = false;
    while (!acyclic) {
      llvm::DenseMap<int, int> denseRoot;
      llvm::SmallVector<int> denseToRoot;
      llvm::SmallVector<int> denseOf(n, -1);
      for (unsigned v = 0; v < n; ++v) {
        auto [it, inserted] = denseRoot.try_emplace(find(v), denseRoot.size());
        if (inserted)
          denseToRoot.push_back(find(v));
        denseOf[v] = it->second;
        (void)inserted;
      }
      const unsigned pc = denseRoot.size();
      llvm::SmallVector<llvm::DenseSet<int>> succ(pc);
      for (unsigned v = 0; v < n; ++v) {
        int rv = denseOf[find(v)];
        for (unsigned p : preds[v]) {
          int rp = denseOf[find(p)];
          if (rp != rv)
            succ[rp].insert(rv);
        }
      }
      llvm::SmallVector<unsigned> indeg(pc, 0u);
      for (unsigned from = 0; from < pc; ++from)
        for (int to : succ[from])
          ++indeg[to];
      llvm::SmallVector<int> ready;
      for (int p = 0; p < static_cast<int>(pc); ++p)
        if (indeg[p] == 0)
          ready.push_back(p);
      unsigned visited = 0;
      while (!ready.empty()) {
        int p = ready.pop_back_val();
        ++visited;
        for (int to : succ[p])
          if (--indeg[to] == 0)
            ready.push_back(to);
      }
      if (visited == pc) {
        acyclic = true;
        break;
      }
      // Union every multi-member SCC into its lowest id (iterative Tarjan).
      enum : unsigned char { kUnvisited = 0, kOpen = 1, kDone = 2 };
      llvm::SmallVector<unsigned char> state(pc, kUnvisited);
      llvm::SmallVector<unsigned> index(pc, 0u), low(pc, 0u);
      llvm::SmallVector<int> onStack(pc, 0);
      llvm::SmallVector<int> tstack;
      llvm::SmallVector<int> frames;
      llvm::SmallVector<unsigned> frameNext(pc, 0u);
      unsigned nextIndex = 0;
      bool unioned = false;
      for (int root = 0; root < static_cast<int>(pc); ++root) {
        if (state[root] != kUnvisited)
          continue;
        frames.push_back(root);
        state[root] = kOpen;
        index[root] = low[root] = nextIndex++;
        tstack.push_back(root);
        onStack[root] = 1;
        while (!frames.empty()) {
          int v = frames.back();
          if (frameNext[v] < succ[v].size()) {
            auto it = succ[v].begin();
            std::advance(it, frameNext[v]++);
            int w = *it;
            if (state[w] == kUnvisited) {
              state[w] = kOpen;
              index[w] = low[w] = nextIndex++;
              tstack.push_back(w);
              onStack[w] = 1;
              frames.push_back(w);
            } else if (onStack[w]) {
              low[v] = std::min(low[v], index[w]);
            }
            continue;
          }
          frames.pop_back();
          if (low[v] == index[v]) {
            llvm::SmallVector<int> members;
            int w;
            do {
              w = tstack.back();
              tstack.pop_back();
              onStack[w] = 0;
              members.push_back(w);
            } while (w != v);
            if (members.size() > 1) {
              // members are dense ids; map back to union-find roots.
              int keepDense = *std::min_element(members.begin(), members.end());
              int keepRoot = denseToRoot[keepDense];
              for (int m : members) {
                int raw = denseToRoot[m];
                if (raw == keepRoot)
                  continue;
                parent[raw] = keepRoot;
                psize[keepRoot] += psize[raw];
                --partCount;
                unioned = true;
              }
            }
          }
          if (!frames.empty()) {
            int parentF = frames.back();
            low[parentF] = std::min(low[parentF], low[v]);
          }
          state[v] = kDone;
        }
      }
      if (!unioned) {
        return failure();
      }
    }
    // Emit assignments.
    llvm::DenseMap<int, unsigned> rootId;
    for (unsigned v = 0; v < n; ++v) {
      int r = find(v);
      auto [it, inserted] = rootId.try_emplace(r, rootId.size());
      plan.partitions[globalOf[v]] = it->second;
      (void)inserted;
    }
    plan.partitionCount = rootId.size();
  }
  (void)coneOf;
  (void)cones;
  (void)slotOrderLocal;
  (void)testEpoch;
  return plan;
}

std::unique_ptr<Pass> createCombPartitionPass(uint64_t targetSize) {
  return std::make_unique<CombPartitionPass>(targetSize);
}

} // namespace pyc
