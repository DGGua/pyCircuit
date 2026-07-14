#include "pyc/Emit/CppPlacement.h"

#include "mlir/IR/Attributes.h"
#include "mlir/IR/BuiltinAttributes.h"
#include "mlir/IR/BuiltinOps.h"
#include "mlir/IR/BuiltinTypes.h"
#include "mlir/IR/Operation.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <queue>
#include <vector>

using namespace mlir;

namespace pyc {

namespace {

static Operation *definingOp(Value v) { return v.getDefiningOp(); }

static void setOnDefiningOp(Value v, CppStorageKind kind, StringRef owner, StringRef shard) {
  Operation *op = definingOp(v);
  if (!op)
    return;
  auto *ctx = op->getContext();
  op->setAttr(kCppStorageAttr, StringAttr::get(ctx, kind == CppStorageKind::Local ? "local" : "struct"));
  if (!owner.empty())
    op->setAttr(kCppOwnerAttr, StringAttr::get(ctx, owner));
  else
    op->removeAttr(kCppOwnerAttr);
  if (!shard.empty())
    op->setAttr(kCppShardAttr, StringAttr::get(ctx, shard));
  else
    op->removeAttr(kCppShardAttr);
}

static std::string cppTypeForWire(Type ty) {
  if (isa<pyc::ClockType>(ty) || isa<pyc::ResetType>(ty))
    return "pyc::cpp::Wire<1>";
  if (auto intTy = dyn_cast<IntegerType>(ty))
    return "pyc::cpp::Wire<" + std::to_string(intTy.getWidth()) + ">";
  return "pyc::cpp::Wire<1>";
}

static std::string methodForCombPart(unsigned combIdx, unsigned partIdx, bool hasParts) {
  if (hasParts)
    return "eval_comb_" + std::to_string(combIdx) + "_part_" + std::to_string(partIdx);
  return "eval_comb_" + std::to_string(combIdx);
}

// Approximate register / transfer cost used when scoring live wires and cuts.
// Wider integers cost more (ceil(width/64) limbs) so cuts prefer thinner values.
static uint64_t placementWeight(Value v) {
  if (auto intTy = dyn_cast<IntegerType>(v.getType()))
    return 1 + (intTy.getWidth() + 63) / 64;
  return 1;
}

// True when `v` is defined inside `comb` and every use stays in that Comb body
// (not Yield, not escaped). Only these values can become method-local and pay
// into crossing / live-range costs.
static bool isLocalizableCombValue(Value v, pyc::CombOp comb) {
  if (!v.getDefiningOp() || v.getDefiningOp()->getParentOfType<pyc::CombOp>() != comb)
    return false;
  for (OpOperand &use : v.getUses()) {
    Operation *user = use.getOwner();
    if (isa<pyc::YieldOp>(user) || user->getParentOfType<pyc::CombOp>() != comb)
      return false;
  }
  return true;
}

// Reorder Comb body ops while preserving SSA edges to shrink live wires before
// chunking. Ready nodes are ranked by remaining work/depth first, then by
// live-weight delta (prefer closing heavy wires), then by stable original index.
// Falls back to `original` if the dependency graph cannot produce a total order.
static llvm::SmallVector<Operation *> localityAwareTopologicalOrder(
    pyc::CombOp comb, ArrayRef<Operation *> original) {
  const unsigned n = original.size();
  llvm::DenseMap<Operation *, unsigned> nodeIndex;
  for (auto [i, op] : llvm::enumerate(original))
    nodeIndex.try_emplace(op, static_cast<unsigned>(i));

  // Build intra-comb SSA dependence graph.
  llvm::SmallVector<llvm::SmallVector<unsigned>> successors(n);
  llvm::SmallVector<unsigned> indegree(n, 0);
  for (auto [i, op] : llvm::enumerate(original)) {
    llvm::DenseSet<unsigned> dependencies;
    for (Value operand : op->getOperands()) {
      auto it = nodeIndex.find(operand.getDefiningOp());
      if (it != nodeIndex.end())
        dependencies.insert(it->second);
    }
    indegree[i] = dependencies.size();
    for (unsigned dependency : dependencies)
      successors[dependency].push_back(i);
  }

  // Static shortest-completion estimate. The saturated sum distinguishes a
  // short narrow chain from a shallow but wide fanout; depth breaks saturated
  // ties. The original body is already in SSA dominance order.
  constexpr uint64_t kWorkLimit = std::numeric_limits<uint64_t>::max() / 4;
  llvm::SmallVector<uint64_t> completionWork(n, 1);
  llvm::SmallVector<unsigned> remainingDepth(n, 1);
  for (unsigned i = n; i-- > 0;) {
    uint64_t work = 1;
    unsigned depth = 1;
    for (unsigned successor : successors[i]) {
      work = std::min(kWorkLimit, work + completionWork[successor]);
      depth = std::max(depth, 1 + remainingDepth[successor]);
    }
    completionWork[i] = work;
    remainingDepth[i] = depth;
  }

  llvm::DenseMap<Value, unsigned> remainingUses;
  for (Operation *op : original)
    for (Value operand : op->getOperands())
      if (nodeIndex.count(operand.getDefiningOp()))
        ++remainingUses[operand];

  // Priority-queue key: smaller work/depth first; then smaller liveDelta
  // (close more weight than open); then larger closedWeight; then earlier index.
  struct ReadyCandidate {
    uint64_t work;
    unsigned depth;
    int64_t liveDelta;
    uint64_t closedWeight;
    unsigned index;
  };
  auto worse = [](const ReadyCandidate &a, const ReadyCandidate &b) {
    if (a.work != b.work)
      return a.work > b.work;
    if (a.depth != b.depth)
      return a.depth > b.depth;
    if (a.liveDelta != b.liveDelta)
      return a.liveDelta > b.liveDelta;
    if (a.closedWeight != b.closedWeight)
      return a.closedWeight < b.closedWeight;
    return a.index > b.index;
  };
  std::priority_queue<ReadyCandidate, std::vector<ReadyCandidate>, decltype(worse)> ready(worse);

  auto makeCandidate = [&](unsigned index) {
    Operation *op = original[index];
    uint64_t opened = 0;
    uint64_t closed = 0;
    // Values this op newly makes live.
    for (Value result : op->getResults())
      if (isLocalizableCombValue(result, comb) &&
          remainingUses.lookup(result) != 0)
        opened += placementWeight(result);

    // Values whose last remaining use is consumed by this op.
    llvm::DenseMap<Value, unsigned> usesHere;
    for (Value operand : op->getOperands())
      if (nodeIndex.count(operand.getDefiningOp()))
        ++usesHere[operand];
    for (auto &entry : usesHere)
      if (remainingUses.lookup(entry.first) == entry.second)
        closed += placementWeight(entry.first);

    return ReadyCandidate{completionWork[index], remainingDepth[index],
                          static_cast<int64_t>(opened) -
                              static_cast<int64_t>(closed),
                          closed, index};
  };

  for (unsigned i = 0; i < n; ++i)
    if (indegree[i] == 0)
      ready.push(makeCandidate(i));

  // Kahn-style scheduling over the ready set.
  llvm::SmallVector<Operation *> ordered;
  ordered.reserve(n);
  while (!ready.empty()) {
    unsigned selected = ready.top().index;
    ready.pop();
    Operation *selectedOp = original[selected];
    ordered.push_back(selectedOp);
    for (Value operand : selectedOp->getOperands()) {
      auto it = remainingUses.find(operand);
      if (it != remainingUses.end() && it->second != 0)
        --it->second;
    }
    for (unsigned successor : successors[selected])
      if (--indegree[successor] == 0)
        ready.push(makeCandidate(successor));
  }

  if (ordered.size() != n)
    return llvm::SmallVector<Operation *>(original.begin(), original.end());
  return ordered;
}

// Choose exclusive part end indices for `order` that minimize total weighted
// cut cost. `maxChunkNodes` is a per-part size cap (TU budget), not a fill
// target: parts may be shorter. Uses the fewest parts K = ceil(n / M) so that
// definition regions stay as large as the cap allows, which favors fewer
// crosses under the localizable-cut metric. Returns ends like [e0, ..., n].
// Falls back to fixed-size ends if the DP cannot reach a valid cover.
static llvm::SmallVector<unsigned>
optimalPartEnds(pyc::CombOp comb, ArrayRef<Operation *> order, unsigned maxChunkNodes) {
  const unsigned n = order.size();
  if (n == 0)
    return {};
  const unsigned parts = (n + maxChunkNodes - 1) / maxChunkNodes;
  if (parts == 1)
    return {n};

  llvm::DenseMap<Operation *, unsigned> position;
  for (auto [i, op] : llvm::enumerate(order))
    position.try_emplace(op, static_cast<unsigned>(i));

  // lastUse[v] = last consumer index in `order` (inclusive).
  llvm::DenseMap<Value, unsigned> lastUse;
  llvm::DenseMap<Value, uint64_t> weights;
  for (auto [i, op] : llvm::enumerate(order)) {
    for (Value result : op->getResults()) {
      if (!isLocalizableCombValue(result, comb))
        continue;
      unsigned last = i;
      for (OpOperand &use : result.getUses()) {
        auto it = position.find(use.getOwner());
        if (it != position.end())
          last = std::max(last, it->second);
      }
      lastUse[result] = last;
      weights[result] = placementWeight(result);
    }
  }

  // Represent each block as size = M - deficit. With minimum part count,
  // slack = K*M - n satisfies 0 <= slack < M. Enumerate every deficit so the
  // search over legal part lengths is exact (minimize weighted cross).
  const unsigned slack = parts * maxChunkNodes - n;
  std::vector<unsigned> deficits;
  deficits.reserve(slack + 1);
  for (unsigned deficit = 0; deficit <= slack; ++deficit)
    deficits.push_back(deficit);
  const unsigned zeroState = 0;
  const unsigned finalState = slack;
  const uint64_t inf = std::numeric_limits<uint64_t>::max() / 4;
  std::vector<uint64_t> previous(deficits.size(), inf), current(deficits.size(), inf);
  std::vector<std::vector<unsigned>> predecessor(
      parts + 1,
      std::vector<unsigned>(deficits.size(), std::numeric_limits<unsigned>::max()));
  previous[zeroState] = 0;

  // DP: minimize total cut weight over `parts` blocks whose deficits sum to
  // slack (each part size <= maxChunkNodes).
  for (unsigned part = 1; part <= parts; ++part) {
    std::fill(current.begin(), current.end(), inf);
    for (auto [deficitState, deficit] : llvm::enumerate(deficits)) {
      unsigned end = part * maxChunkNodes - deficit;
      if (end == 0 || end > n)
        continue;

      // crossingByLength[len] is the exact weight of values defined in the
      // candidate block [end-len,end) and used at or after end.
      unsigned maxLength = std::min(maxChunkNodes, end);
      std::vector<uint64_t> crossingByLength(maxLength + 1, 0);
      uint64_t crossing = 0;
      for (unsigned length = 1; length <= maxLength; ++length) {
        Operation *definition = order[end - length];
        for (Value result : definition->getResults()) {
          auto useIt = lastUse.find(result);
          if (useIt != lastUse.end() && useIt->second >= end)
            crossing += weights.lookup(result);
        }
        crossingByLength[length] = crossing;
      }

      for (auto [previousState, previousDeficit] : llvm::enumerate(deficits)) {
        if (previousDeficit > deficit || previous[previousState] == inf)
          continue;
        unsigned begin = (part - 1) * maxChunkNodes - previousDeficit;
        unsigned length = end - begin;
        if (length == 0 || length > maxLength)
          continue;
        uint64_t candidate = previous[previousState] + crossingByLength[length];
        if (candidate < current[deficitState]) {
          current[deficitState] = candidate;
          predecessor[part][deficitState] = previousState;
        }
      }
    }
    previous.swap(current);
  }

  // Unreachable final deficit: fall back to uniform maxChunkNodes cuts.
  if (previous[finalState] == inf) {
    llvm::SmallVector<unsigned> fixed;
    for (unsigned end = maxChunkNodes; end < n; end += maxChunkNodes)
      fixed.push_back(end);
    fixed.push_back(n);
    return fixed;
  }

  // Reconstruct exclusive ends from the chosen deficit per part.
  llvm::SmallVector<unsigned> reversed;
  unsigned state = finalState;
  for (unsigned part = parts; part > 0; --part) {
    unsigned deficit = deficits[state];
    reversed.push_back(part * maxChunkNodes - deficit);
    state = predecessor[part][state];
  }
  std::reverse(reversed.begin(), reversed.end());
  return reversed;
}

// Count localizable values whose uses leave the defining part, plus the sum of
// their placementWeight. Returned as {crossingValueCount, weightedCutCost}.
static std::pair<unsigned, uint64_t>
crossingStats(pyc::CombOp comb, ArrayRef<Operation *> order,
              ArrayRef<unsigned> partEnds) {
  llvm::DenseMap<Operation *, unsigned> partForOp;
  unsigned begin = 0;
  for (auto [part, end] : llvm::enumerate(partEnds)) {
    for (unsigned i = begin; i < end; ++i)
      partForOp[order[i]] = part;
    begin = end;
  }

  unsigned values = 0;
  uint64_t weight = 0;
  for (Operation *op : order) {
    unsigned definitionPart = partForOp.lookup(op);
    for (Value result : op->getResults()) {
      if (!isLocalizableCombValue(result, comb))
        continue;
      bool crosses = false;
      for (OpOperand &use : result.getUses()) {
        auto it = partForOp.find(use.getOwner());
        if (it != partForOp.end() && it->second != definitionPart) {
          crosses = true;
          break;
        }
      }
      if (crosses) {
        ++values;
        weight += placementWeight(result);
      }
    }
  }
  return {values, weight};
}

// Assign each Comb body op to an eval_comb_* / eval_comb_*_part_* method.
// When chunking, compare locality-aware schedule + DP cuts against fixed-size
// chunks, reorder the body to the winner, and stamp pyc.cpp.method (+ cut attrs).
static void assignCombOpMethods(pyc::CombOp comb, unsigned combIdx, unsigned combChunkNodes,
                                llvm::DenseMap<Operation *, std::string> &opToMethod,
                                llvm::DenseMap<pyc::CombOp, std::string> &combWrappers) {
  comb->setAttr(kCppCombIndexAttr,
                IntegerAttr::get(IntegerType::get(comb.getContext(), 64), combIdx));
  combWrappers[comb] = "eval_comb_" + std::to_string(combIdx);

  Block &b = comb.getBody().front();
  llvm::SmallVector<Operation *> combOps;
  for (Operation &op : b) {
    if (isa<pyc::YieldOp>(op))
      break;
    combOps.push_back(&op);
  }

  // Small combs stay in one method: eval_comb_N.
  const bool chunk = combOps.size() > combChunkNodes;
  if (!chunk) {
    std::string m = methodForCombPart(combIdx, 0, false);
    for (Operation *op : combOps) {
      opToMethod[op] = m;
      op->setAttr(kCppMethodAttr, StringAttr::get(op->getContext(), m));
    }
    return;
  }

  // Large combs are split into eval_comb_N_part_K shards.
  // Prefer a locality-aware schedule that keeps cut wires small; if it does not
  // beat fixed-size chunking, fall back to the original body order.
  llvm::SmallVector<Operation *> ordered = localityAwareTopologicalOrder(comb, combOps);
  llvm::SmallVector<unsigned> partEnds = optimalPartEnds(comb, ordered, combChunkNodes);

  // Baseline: preserve original SSA order and cut every combChunkNodes ops.
  llvm::SmallVector<unsigned> fixedPartEnds;
  for (unsigned end = combChunkNodes; end < combOps.size(); end += combChunkNodes)
    fixedPartEnds.push_back(end);
  fixedPartEnds.push_back(combOps.size());

  // Stats are (crossing value count, weighted cut cost). Prefer lower weight,
  // then fewer crossing values when weights tie.
  auto fixedStats = crossingStats(comb, combOps, fixedPartEnds);
  auto scheduledStats = crossingStats(comb, ordered, partEnds);
  if (scheduledStats.second > fixedStats.second ||
      (scheduledStats.second == fixedStats.second &&
       scheduledStats.first > fixedStats.first)) {
    ordered.assign(combOps.begin(), combOps.end());
    partEnds.assign(fixedPartEnds.begin(), fixedPartEnds.end());
    scheduledStats = fixedStats;
  }

  // Diagnostic attrs for comparing chunking quality in emit stats / dumps.
  auto *ctx = comb.getContext();
  comb->setAttr("pyc.cpp.fixed_cross_values",
                IntegerAttr::get(IntegerType::get(ctx, 64), fixedStats.first));
  comb->setAttr("pyc.cpp.scheduled_cross_values",
                IntegerAttr::get(IntegerType::get(ctx, 64), scheduledStats.first));
  comb->setAttr("pyc.cpp.scheduled_cut_weight",
                IntegerAttr::get(IntegerType::get(ctx, 64), scheduledStats.second));

  // Materialize the chosen order in the Comb body.
  Operation *terminator = b.getTerminator();
  for (Operation *op : ordered)
    op->moveBefore(terminator);

  // Stamp each op with its part method so the C++ emitter can split the TU.
  unsigned begin = 0;
  for (auto [partIdx, end] : llvm::enumerate(partEnds)) {
    std::string m = methodForCombPart(combIdx, partIdx, true);
    for (unsigned i = begin; i < end; ++i) {
      opToMethod[ordered[i]] = m;
      ordered[i]->setAttr(kCppMethodAttr, StringAttr::get(ordered[i]->getContext(), m));
    }
    begin = end;
  }
}

static llvm::SmallVector<pyc::CombOp> collectSortedTopLevelCombs(func::FuncOp f) {
  llvm::SmallVector<pyc::CombOp> combs;
  if (f.getBody().empty())
    return combs;
  Block &top = f.getBody().front();
  for (Operation &op : top)
    if (auto comb = dyn_cast<pyc::CombOp>(op))
      combs.push_back(comb);
  return combs;
}

static bool pinToStruct(Value v) {
  if (!v.getDefiningOp())
    return true;

  Operation *def = v.getDefiningOp();
  if (isa<pyc::CombOp>(def))
    return true;
  if (isa<pyc::RegOp, pyc::InstanceOp, pyc::FifoOp, pyc::ByteMemOp, pyc::SyncMemOp, pyc::SyncMemDPOp,
            pyc::AsyncFifoOp, pyc::CdcSyncOp>(def))
    return true;

  if (def->getParentOfType<pyc::CombOp>()) {
    for (OpOperand &use : v.getUses()) {
      Operation *user = use.getOwner();
      if (!user->getParentOfType<pyc::CombOp>())
        return true;
    }
  }
  return false;
}

static StringRef ownerOfValue(Value v) {
  StringRef owner = getValueCppOwner(v);
  return owner.empty() ? StringRef("core") : owner;
}

static StringRef methodForUserOp(Operation *user,
                                 const llvm::DenseMap<Operation *, std::string> &opToMethod,
                                 const llvm::DenseMap<pyc::CombOp, std::string> &combWrappers) {
  if (auto it = opToMethod.find(user); it != opToMethod.end())
    return it->second;
  if (isa<pyc::YieldOp>(user)) {
    if (auto comb = user->getParentOfType<pyc::CombOp>()) {
      if (auto it = combWrappers.find(comb); it != combWrappers.end())
        return it->second;
    }
  }
  return "core";
}

// Phase-1 scheduling entry: walk top-level Combs and assign method ownership.
static void buildCombMethodMaps(func::FuncOp f, unsigned combChunkNodes,
                                llvm::DenseMap<Operation *, std::string> &opToMethod,
                                llvm::DenseMap<pyc::CombOp, std::string> &combWrappers) {
  llvm::SmallVector<pyc::CombOp> combs = collectSortedTopLevelCombs(f);
  for (auto [i, comb] : llvm::enumerate(combs))
    assignCombOpMethods(comb, static_cast<unsigned>(i), combChunkNodes, opToMethod, combWrappers);
}

// Rebuild op→method maps from attrs written by scheduleCppCombMethods / buildCombMethodMaps.
// Used by localization so it does not re-chunk or reorder Comb bodies.
static void readCombMethodMaps(func::FuncOp f,
                               llvm::DenseMap<Operation *, std::string> &opToMethod,
                               llvm::DenseMap<pyc::CombOp, std::string> &combWrappers) {
  llvm::SmallVector<pyc::CombOp> combs = collectSortedTopLevelCombs(f);
  for (auto [i, comb] : llvm::enumerate(combs)) {
    combWrappers[comb] = "eval_comb_" + std::to_string(i);
    for (Operation &op : comb.getBody().front().without_terminator()) {
      if (auto owner = op.getAttrOfType<StringAttr>(kCppMethodAttr))
        opToMethod[&op] = owner.getValue().str();
    }
  }
}

} // namespace

CppStorageKind getValueCppStorage(Value v) {
  Operation *op = definingOp(v);
  if (!op)
    return CppStorageKind::Struct;
  if (auto a = op->getAttrOfType<StringAttr>(kCppStorageAttr)) {
    if (a.getValue() == "local")
      return CppStorageKind::Local;
  }
  return CppStorageKind::Struct;
}

StringRef getValueCppOwner(Value v) {
  Operation *op = definingOp(v);
  if (!op)
    return {};
  if (auto a = op->getAttrOfType<StringAttr>(kCppOwnerAttr))
    return a.getValue();
  return {};
}

void setValueCppPlacement(Value v, CppStorageKind kind, StringRef owner, StringRef shard) {
  setOnDefiningOp(v, kind, owner, shard);
}

bool CppEmitterPlacementState::emitLocalDeclIfNeeded(Value v, Type ty, StringRef name,
                                                   llvm::raw_ostream &os, unsigned indentSpaces) {
  if (getValueCppStorage(v) != CppStorageKind::Local)
    return false;
  StringRef owner = getValueCppOwner(v);
  if (!owner.empty() && owner != currentMethod)
    return false;
  if (!declaredLocals.insert(v).second)
    return false;
  for (unsigned i = 0; i < indentSpaces; ++i)
    os << ' ';
  os << cppTypeForWire(ty) << " " << name << "{};\n";
  return true;
}

void scheduleCppCombMethods(func::FuncOp f, unsigned combChunkNodes) {
  if (combChunkNodes == 0)
    return;

  llvm::DenseMap<Operation *, std::string> opToMethod;
  llvm::DenseMap<pyc::CombOp, std::string> combWrappers;
  buildCombMethodMaps(f, combChunkNodes, opToMethod, combWrappers);

  f.walk([&](Operation *op) {
    if (op->getParentOfType<pyc::CombOp>() == nullptr)
      return;
    if (isa<pyc::YieldOp>(op))
      return;
    auto it = opToMethod.find(op);
    if (it == opToMethod.end())
      return;
    for (Value r : op->getResults())
      setValueCppPlacement(r, CppStorageKind::Struct, it->second, "comb");
  });
}

CppPlacementSummary localizeCppCombMembers(func::FuncOp f, unsigned combChunkNodes) {
  CppPlacementSummary summary;

  llvm::DenseMap<Operation *, std::string> opToMethod;
  llvm::DenseMap<pyc::CombOp, std::string> combWrappers;
  if (combChunkNodes > 0)
    readCombMethodMaps(f, opToMethod, combWrappers);
  for (pyc::CombOp comb : collectSortedTopLevelCombs(f)) {
    if (auto attr = comb->getAttrOfType<IntegerAttr>("pyc.cpp.fixed_cross_values"))
      summary.fixedOrderCrossMethod +=
          static_cast<unsigned>(attr.getValue().getZExtValue());
    if (auto attr = comb->getAttrOfType<IntegerAttr>("pyc.cpp.scheduled_cross_values"))
      summary.scheduledCrossMethod +=
          static_cast<unsigned>(attr.getValue().getZExtValue());
    if (auto attr = comb->getAttrOfType<IntegerAttr>("pyc.cpp.scheduled_cut_weight"))
      summary.scheduledCutWeight += attr.getValue().getZExtValue();
  }

  llvm::SmallVector<Value> candidates;
  f.walk([&](Operation *op) {
    if (op->getParentOfType<pyc::CombOp>() == nullptr)
      return;
    if (isa<pyc::YieldOp>(op))
      return;
    for (Value r : op->getResults())
      candidates.push_back(r);
  });

  for (Value v : candidates) {
    if (pinToStruct(v)) {
      setValueCppPlacement(v, CppStorageKind::Struct, {});
      summary.structMembers++;
      if (!v.getDefiningOp())
        summary.probePinnedStruct++;
      continue;
    }

    Operation *def = v.getDefiningOp();
    if (!def) {
      setValueCppPlacement(v, CppStorageKind::Struct, {});
      summary.structMembers++;
      continue;
    }

    const StringRef ownerM = ownerOfValue(v);
    bool crossMethod = false;
    for (OpOperand &use : v.getUses()) {
      Operation *user = use.getOwner();
      if (methodForUserOp(user, opToMethod, combWrappers) != ownerM) {
        crossMethod = true;
        break;
      }
    }
    if (crossMethod) {
      setValueCppPlacement(v, CppStorageKind::Struct, ownerM, "comb");
      summary.structMembers++;
      summary.promotedCrossMethod++;
      continue;
    }

    setValueCppPlacement(v, CppStorageKind::Local, ownerM, "comb");
    summary.localInMethod++;
  }

  f.walk([&](Operation *op) {
    if (op->getParentOfType<pyc::CombOp>() != nullptr)
      return;
    for (Value r : op->getResults()) {
      if (getValueCppStorage(r) == CppStorageKind::Local)
        continue;
      setValueCppPlacement(r, CppStorageKind::Struct, {});
      summary.structMembers++;
    }
  });

  return summary;
}

CppPlacementSummary runCppMemberPlacement(func::FuncOp f, unsigned combChunkNodes) {
  scheduleCppCombMethods(f, combChunkNodes);
  return localizeCppCombMembers(f, combChunkNodes);
}

void setModuleCombChunkNodes(ModuleOp module, unsigned combChunkNodes) {
  auto *ctx = module.getContext();
  module->setAttr(kCppCombChunkNodesAttr,
                  IntegerAttr::get(IntegerType::get(ctx, 64), combChunkNodes));
}

std::optional<unsigned> getModuleCombChunkNodes(ModuleOp module) {
  auto attr = module->getAttrOfType<IntegerAttr>(kCppCombChunkNodesAttr);
  if (!attr)
    return std::nullopt;
  return static_cast<unsigned>(attr.getValue().getZExtValue());
}

void setFuncPlacementSummary(func::FuncOp f, const CppPlacementSummary &summary) {
  auto *ctx = f.getContext();
  llvm::SmallVector<NamedAttribute, 8> fields;
  fields.emplace_back(StringAttr::get(ctx, "struct_members"),
                      IntegerAttr::get(IntegerType::get(ctx, 64), summary.structMembers));
  fields.emplace_back(StringAttr::get(ctx, "local_in_method"),
                      IntegerAttr::get(IntegerType::get(ctx, 64), summary.localInMethod));
  fields.emplace_back(StringAttr::get(ctx, "promoted_cross_method"),
                      IntegerAttr::get(IntegerType::get(ctx, 64), summary.promotedCrossMethod));
  fields.emplace_back(StringAttr::get(ctx, "probe_pinned_struct"),
                      IntegerAttr::get(IntegerType::get(ctx, 64), summary.probePinnedStruct));
  fields.emplace_back(StringAttr::get(ctx, "fixed_order_cross_method"),
                      IntegerAttr::get(IntegerType::get(ctx, 64), summary.fixedOrderCrossMethod));
  fields.emplace_back(StringAttr::get(ctx, "scheduled_cross_method"),
                      IntegerAttr::get(IntegerType::get(ctx, 64), summary.scheduledCrossMethod));
  fields.emplace_back(StringAttr::get(ctx, "scheduled_cut_weight"),
                      IntegerAttr::get(IntegerType::get(ctx, 64), summary.scheduledCutWeight));
  f->setAttr(kCppPlacementSummaryAttr, DictionaryAttr::get(ctx, fields));
}

static std::optional<uint64_t> readSummaryField(func::FuncOp f, StringRef key) {
  auto dict = f->getAttrOfType<DictionaryAttr>(kCppPlacementSummaryAttr);
  if (!dict)
    return std::nullopt;
  auto attr = dict.get(key);
  if (!attr)
    return std::nullopt;
  if (auto intAttr = dyn_cast<IntegerAttr>(attr))
    return intAttr.getValue().getZExtValue();
  return std::nullopt;
}

std::optional<CppPlacementSummary> getFuncPlacementSummary(func::FuncOp f) {
  auto structMembers = readSummaryField(f, "struct_members");
  if (!structMembers)
    return std::nullopt;
  CppPlacementSummary summary;
  summary.structMembers = static_cast<unsigned>(*structMembers);
  if (auto v = readSummaryField(f, "local_in_method"))
    summary.localInMethod = static_cast<unsigned>(*v);
  if (auto v = readSummaryField(f, "promoted_cross_method"))
    summary.promotedCrossMethod = static_cast<unsigned>(*v);
  if (auto v = readSummaryField(f, "probe_pinned_struct"))
    summary.probePinnedStruct = static_cast<unsigned>(*v);
  if (auto v = readSummaryField(f, "fixed_order_cross_method"))
    summary.fixedOrderCrossMethod = static_cast<unsigned>(*v);
  if (auto v = readSummaryField(f, "scheduled_cross_method"))
    summary.scheduledCrossMethod = static_cast<unsigned>(*v);
  if (auto v = readSummaryField(f, "scheduled_cut_weight"))
    summary.scheduledCutWeight = *v;
  return summary;
}

CppPlacementSummary accumulateModulePlacementSummary(ModuleOp module) {
  CppPlacementSummary totals;
  for (auto f : module.getOps<func::FuncOp>()) {
    if (f.isDeclaration())
      continue;
    if (auto summary = getFuncPlacementSummary(f)) {
      totals.structMembers += summary->structMembers;
      totals.localInMethod += summary->localInMethod;
      totals.promotedCrossMethod += summary->promotedCrossMethod;
      totals.probePinnedStruct += summary->probePinnedStruct;
      totals.fixedOrderCrossMethod += summary->fixedOrderCrossMethod;
      totals.scheduledCrossMethod += summary->scheduledCrossMethod;
      totals.scheduledCutWeight += summary->scheduledCutWeight;
    }
  }
  return totals;
}

void CppEmitterPlacementState::emitValueAssign(Value result, Type ty, StringRef name, StringRef expr,
                                               llvm::raw_ostream &os, unsigned indentSpaces) {
  for (unsigned i = 0; i < indentSpaces; ++i)
    os << ' ';
  if (getValueCppStorage(result) != CppStorageKind::Local) {
    os << name << " = " << expr << ";\n";
    return;
  }
  StringRef owner = getValueCppOwner(result);
  if (!owner.empty() && owner != currentMethod) {
    // Placement owner indices can disagree with emitter comb order; still declare
    // the SSA temp in the method that defines it.
    if (declaredLocals.insert(result).second)
      os << cppTypeForWire(ty) << " " << name << " = " << expr << ";\n";
    else
      os << name << " = " << expr << ";\n";
    return;
  }
  // SSA: one defining assign per Value; further uses only read `name`. If an operand
  // prep emitted `{}` via emitLocalDeclIfNeeded, the defining op takes this branch.
  if (declaredLocals.insert(result).second)
    os << cppTypeForWire(ty) << " " << name << " = " << expr << ";\n";
  else
    os << name << " = " << expr << ";\n";
}

} // namespace pyc
