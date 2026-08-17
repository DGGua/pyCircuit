#include "pyc/Transforms/Passes.h"

#include "pyc/Dialect/PYC/PYCOps.h"

#include "mlir/Dialect/Func/IR/FuncOps.h"
#include "mlir/IR/Builders.h"
#include "mlir/IR/BuiltinAttributes.h"
#include "mlir/IR/IRMapping.h"
#include "mlir/Pass/Pass.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/Support/CommandLine.h"

#include <algorithm>
#include <cstdint>
#include <functional>
#include <limits>
#include <map>
#include <queue>
#include <vector>

using namespace mlir;

namespace pyc {
namespace {

constexpr llvm::StringLiteral kParentIdAttr = "pyc.partition.parent_id";
constexpr llvm::StringLiteral kPartIdAttr = "pyc.partition.part_id";
constexpr llvm::StringLiteral kPartCountAttr = "pyc.partition.part_count";
constexpr llvm::StringLiteral kPlanVersionAttr = "pyc.partition.plan_version";
constexpr llvm::StringLiteral kWorkAttr = "pyc.partition.work";
constexpr llvm::StringLiteral kMaxNodesAttr = "pyc.partition.max_nodes";
constexpr llvm::StringLiteral kPlanVersion = "gsim-contiguous-dp-v1";

static bool hasAnyPartitionAttribute(Operation *op) {
  return op->hasAttr(kParentIdAttr) || op->hasAttr(kPartIdAttr) ||
         op->hasAttr(kPartCountAttr) || op->hasAttr(kPlanVersionAttr) ||
         op->hasAttr(kWorkAttr) || op->hasAttr(kMaxNodesAttr);
}

// pyc.comb bodies are already SSA-ordered, but compute the order explicitly so
// the partition algorithm has a deterministic graph contract.  Original body
// order breaks ties, matching GSIM's ordered topological traversal.
static FailureOr<llvm::SmallVector<Operation *>>
stableTopologicalOrder(pyc::CombOp comb) {
  llvm::SmallVector<Operation *> operations;
  for (Operation &op : comb.getBody().front().without_terminator())
    operations.push_back(&op);

  llvm::DenseMap<Operation *, unsigned> index;
  for (auto [i, op] : llvm::enumerate(operations))
    index[op] = static_cast<unsigned>(i);

  llvm::SmallVector<llvm::SmallVector<unsigned>> successors(operations.size());
  llvm::SmallVector<unsigned> indegree(operations.size(), 0);
  for (auto [i, op] : llvm::enumerate(operations)) {
    llvm::DenseSet<unsigned> dependencies;
    for (Value operand : op->getOperands()) {
      auto it = index.find(operand.getDefiningOp());
      if (it != index.end())
        dependencies.insert(it->second);
    }
    indegree[i] = dependencies.size();
    for (unsigned dependency : dependencies)
      successors[dependency].push_back(static_cast<unsigned>(i));
  }

  std::priority_queue<unsigned, std::vector<unsigned>, std::greater<unsigned>>
      ready;
  for (unsigned i = 0; i < operations.size(); ++i)
    if (indegree[i] == 0)
      ready.push(i);

  llvm::SmallVector<Operation *> ordered;
  ordered.reserve(operations.size());
  while (!ready.empty()) {
    unsigned current = ready.top();
    ready.pop();
    ordered.push_back(operations[current]);
    for (unsigned successor : successors[current])
      if (--indegree[successor] == 0)
        ready.push(successor);
  }

  if (ordered.size() != operations.size()) {
    comb.emitOpError("cannot partition a cyclic pyc.comb dependence graph");
    return failure();
  }
  return ordered;
}

// A compact union-find representation of GSIM's coarsened SuperNodes.  Work is
// the number of dialect operations; every merge is capped so an atom can never
// make the final max-nodes constraint unsatisfiable.
class CoarsenedGroups {
public:
  explicit CoarsenedGroups(unsigned count)
      : parent(count), work(count, 1), firstMember(count) {
    for (unsigned i = 0; i < count; ++i)
      parent[i] = firstMember[i] = i;
  }

  unsigned find(unsigned node) {
    if (parent[node] != node)
      parent[node] = find(parent[node]);
    return parent[node];
  }

  void mergeInto(unsigned source, unsigned target) {
    source = find(source);
    target = find(target);
    if (source == target)
      return;
    parent[source] = target;
    work[target] += work[source];
    firstMember[target] = std::min(firstMember[target], firstMember[source]);
  }

  unsigned getWork(unsigned node) { return work[find(node)]; }
  unsigned getFirstMember(unsigned node) { return firstMember[find(node)]; }

private:
  std::vector<unsigned> parent;
  std::vector<unsigned> work;
  std::vector<unsigned> firstMember;
};

struct CoarsenedGraph {
  llvm::SmallVector<unsigned> topologicalRoots;
  std::vector<llvm::DenseSet<unsigned>> predecessors;
  std::vector<llvm::DenseSet<unsigned>> successors;
};

// Rebuild the quotient graph after a coarsening phase.  Stable member order is
// the tie-breaker for Kahn's algorithm, so pointer addresses and DenseSet
// iteration order never affect the plan.
static FailureOr<CoarsenedGraph>
buildCoarsenedGraph(ArrayRef<Operation *> order, CoarsenedGroups &groups) {
  const unsigned n = order.size();
  CoarsenedGraph graph;
  graph.predecessors.resize(n);
  graph.successors.resize(n);

  llvm::DenseMap<Operation *, unsigned> position;
  for (auto [i, op] : llvm::enumerate(order))
    position[op] = static_cast<unsigned>(i);

  for (auto [i, op] : llvm::enumerate(order)) {
    unsigned source = groups.find(static_cast<unsigned>(i));
    for (Value result : op->getResults()) {
      for (OpOperand &use : result.getUses()) {
        auto it = position.find(use.getOwner());
        if (it == position.end())
          continue;
        unsigned target = groups.find(it->second);
        if (source == target)
          continue;
        graph.successors[source].insert(target);
        graph.predecessors[target].insert(source);
      }
    }
  }

  using Ready = std::pair<unsigned, unsigned>; // stable member, root
  std::priority_queue<Ready, std::vector<Ready>, std::greater<Ready>> ready;
  std::vector<unsigned> indegree(n, 0);
  unsigned rootCount = 0;
  for (unsigned i = 0; i < n; ++i) {
    if (groups.find(i) != i)
      continue;
    ++rootCount;
    indegree[i] = graph.predecessors[i].size();
    if (indegree[i] == 0)
      ready.emplace(groups.getFirstMember(i), i);
  }

  while (!ready.empty()) {
    unsigned root = ready.top().second;
    ready.pop();
    graph.topologicalRoots.push_back(root);
    llvm::SmallVector<unsigned> orderedSuccessors(
        graph.successors[root].begin(), graph.successors[root].end());
    llvm::sort(orderedSuccessors, [&](unsigned lhs, unsigned rhs) {
      return groups.getFirstMember(lhs) < groups.getFirstMember(rhs);
    });
    for (unsigned successor : orderedSuccessors) {
      if (--indegree[successor] == 0)
        ready.emplace(groups.getFirstMember(successor), successor);
    }
  }

  if (graph.topologicalRoots.size() != rootCount)
    return failure();
  return graph;
}

static bool hasPath(unsigned source, unsigned target,
                    const CoarsenedGraph &graph) {
  if (source == target)
    return true;
  llvm::SmallVector<unsigned> stack{source};
  llvm::DenseSet<unsigned> visited{source};
  while (!stack.empty()) {
    unsigned current = stack.pop_back_val();
    for (unsigned successor : graph.successors[current]) {
      if (successor == target)
        return true;
      if (visited.insert(successor).second)
        stack.push_back(successor);
    }
  }
  return false;
}

struct CoarsenedOrder {
  llvm::SmallVector<Operation *> operations;
  // Exclusive operation end for each indivisible coarsened atom.
  llvm::SmallVector<unsigned> atomEnds;
};

// Mirror the active GSIM coarsening phases that apply to an ordinary pure DAG:
// mergeOut1, mergeIn1, then mergeSiblings.  Reset/when-specific phases have no
// counterpart inside a pure pyc.comb region.  Quotient-graph cycle checks make
// the reordering contract explicit rather than relying on construction luck.
static FailureOr<CoarsenedOrder>
coarsenOperations(ArrayRef<Operation *> initialOrder, unsigned maxNodes) {
  CoarsenedGroups groups(initialOrder.size());
  if (initialOrder.empty())
    return CoarsenedOrder{};

  // mergeOut1: reverse topological order, folding a single-fanout producer into
  // its consumer when the combined atom stays within the runtime work cap.
  auto graphOr = buildCoarsenedGraph(initialOrder, groups);
  if (failed(graphOr))
    return failure();
  CoarsenedGraph graph = std::move(*graphOr);
  for (unsigned originalRoot : llvm::reverse(graph.topologicalRoots)) {
    unsigned source = groups.find(originalRoot);
    if (source != originalRoot)
      continue;
    llvm::DenseSet<unsigned> currentSuccessors;
    for (unsigned successor : graph.successors[originalRoot]) {
      successor = groups.find(successor);
      if (successor != source)
        currentSuccessors.insert(successor);
    }
    if (currentSuccessors.size() != 1)
      continue;
    unsigned target = *currentSuccessors.begin();
    if (groups.getWork(source) + groups.getWork(target) <= maxNodes)
      groups.mergeInto(source, target);
  }

  // mergeIn1: rebuild after out-degree folding, then fold a single-input
  // consumer into its producer in forward topological order.
  graphOr = buildCoarsenedGraph(initialOrder, groups);
  if (failed(graphOr))
    return failure();
  graph = std::move(*graphOr);
  for (unsigned originalRoot : graph.topologicalRoots) {
    unsigned target = groups.find(originalRoot);
    if (target != originalRoot)
      continue;
    llvm::DenseSet<unsigned> currentPredecessors;
    for (unsigned predecessor : graph.predecessors[originalRoot]) {
      predecessor = groups.find(predecessor);
      if (predecessor != target)
        currentPredecessors.insert(predecessor);
    }
    if (currentPredecessors.size() != 1)
      continue;
    unsigned source = *currentPredecessors.begin();
    if (groups.getWork(source) + groups.getWork(target) <= maxNodes)
      groups.mergeInto(target, source);
  }

  // mergeSiblings: groups with an identical non-empty predecessor set are
  // packed greedily in stable topological order.  The path check corresponds
  // to GSIM's dependency-order safety checks and prevents quotient cycles.
  graphOr = buildCoarsenedGraph(initialOrder, groups);
  if (failed(graphOr))
    return failure();
  graph = std::move(*graphOr);
  std::map<std::vector<unsigned>, std::vector<unsigned>> siblingBuckets;
  for (unsigned root : graph.topologicalRoots) {
    if (graph.predecessors[root].empty())
      continue;
    std::vector<unsigned> key;
    for (unsigned predecessor : graph.predecessors[root])
      key.push_back(groups.getFirstMember(predecessor));
    llvm::sort(key);

    auto &representatives = siblingBuckets[key];
    bool merged = false;
    if (!representatives.empty()) {
      unsigned &candidate = representatives.back();
      candidate = groups.find(candidate);
      unsigned current = groups.find(root);
      if (candidate == current) {
        merged = true;
      } else if (groups.getWork(candidate) + groups.getWork(current) <=
                     maxNodes &&
                 !hasPath(candidate, current, graph) &&
                 !hasPath(current, candidate, graph)) {
        groups.mergeInto(current, candidate);
        merged = true;
      }
    }
    if (!merged)
      representatives.push_back(groups.find(root));
  }

  graphOr = buildCoarsenedGraph(initialOrder, groups);
  if (failed(graphOr))
    return failure();
  graph = std::move(*graphOr);

  CoarsenedOrder result;
  std::vector<llvm::SmallVector<unsigned>> members(initialOrder.size());
  for (unsigned i = 0; i < initialOrder.size(); ++i)
    members[groups.find(i)].push_back(i);
  for (unsigned root : graph.topologicalRoots) {
    // Original order is topological inside an atom and is the deterministic
    // member order used by GSIM after resorting SuperNodes.
    for (unsigned i : members[root])
      result.operations.push_back(initialOrder[i]);
    result.atomEnds.push_back(result.operations.size());
  }
  return result;
}

// Count graph edges leaving [begin,end).  As in GSIM's initial partition, the
// DP minimizes inter-SuperNode communication subject to a hard work cap.  Uses
// by the same operation are one graph edge, consistent with GSIM's adjacency
// sets; a pyc.yield use is the observable edge leaving the original comb.
static uint64_t
intervalBoundaryCost(ArrayRef<Operation *> order,
                     const llvm::DenseMap<Operation *, unsigned> &position,
                     unsigned begin, unsigned end) {
  uint64_t cost = 0;
  for (unsigned i = begin; i < end; ++i) {
    for (Value result : order[i]->getResults()) {
      llvm::DenseSet<Operation *> outsideUsers;
      for (OpOperand &use : result.getUses()) {
        Operation *owner = use.getOwner();
        auto it = position.find(owner);
        if (it == position.end() || it->second < begin || it->second >= end)
          outsideUsers.insert(owner);
      }
      cost += outsideUsers.size();
    }
  }
  return cost;
}

// Dynamic programming over contiguous topological intervals.  The primary
// objective is cut-edge count; ties prefer fewer parts and then an earlier last
// cut, making output stable across runs.
static FailureOr<llvm::SmallVector<unsigned>>
choosePartEnds(ArrayRef<Operation *> order, ArrayRef<unsigned> atomEnds,
               unsigned maxNodes) {
  const unsigned n = order.size();
  if (n == 0)
    return llvm::SmallVector<unsigned>{0};
  if (atomEnds.empty() || atomEnds.back() != n)
    return failure();

  llvm::DenseMap<Operation *, unsigned> position;
  for (auto [i, op] : llvm::enumerate(order))
    position[op] = static_cast<unsigned>(i);

  constexpr uint64_t infinity = std::numeric_limits<uint64_t>::max() / 4;
  const unsigned atomCount = atomEnds.size();
  std::vector<uint64_t> bestCost(atomCount + 1, infinity);
  std::vector<unsigned> bestParts(atomCount + 1,
                                  std::numeric_limits<unsigned>::max());
  std::vector<unsigned> predecessor(atomCount + 1,
                                    std::numeric_limits<unsigned>::max());
  bestCost[0] = 0;
  bestParts[0] = 0;

  for (unsigned endAtom = 1; endAtom <= atomCount; ++endAtom) {
    unsigned end = atomEnds[endAtom - 1];
    for (unsigned beginAtom = endAtom; beginAtom-- > 0;) {
      unsigned begin = beginAtom == 0 ? 0 : atomEnds[beginAtom - 1];
      if (end - begin > maxNodes)
        break;
      if (bestCost[beginAtom] == infinity)
        continue;
      uint64_t boundary = intervalBoundaryCost(order, position, begin, end);
      uint64_t candidateCost = bestCost[beginAtom] + boundary;
      unsigned candidateParts = bestParts[beginAtom] + 1;
      bool better = candidateCost < bestCost[endAtom];
      if (candidateCost == bestCost[endAtom]) {
        better = candidateParts < bestParts[endAtom] ||
                 (candidateParts == bestParts[endAtom] &&
                  beginAtom < predecessor[endAtom]);
      }
      if (better) {
        bestCost[endAtom] = candidateCost;
        bestParts[endAtom] = candidateParts;
        predecessor[endAtom] = beginAtom;
      }
    }
  }

  if (bestCost[atomCount] == infinity)
    return failure();

  llvm::SmallVector<unsigned> reversed;
  for (unsigned endAtom = atomCount; endAtom != 0;
       endAtom = predecessor[endAtom])
    reversed.push_back(atomEnds[endAtom - 1]);
  std::reverse(reversed.begin(), reversed.end());
  return reversed;
}

static void stampPartition(pyc::CombOp comb, uint64_t parentId, uint64_t partId,
                           uint64_t partCount, uint64_t work,
                           uint64_t maxNodes) {
  MLIRContext *context = comb.getContext();
  Type i64 = IntegerType::get(context, 64);
  auto integer = [&](uint64_t value) {
    return IntegerAttr::get(i64, static_cast<int64_t>(value));
  };
  comb->setAttr(kParentIdAttr, integer(parentId));
  comb->setAttr(kPartIdAttr, integer(partId));
  comb->setAttr(kPartCountAttr, integer(partCount));
  comb->setAttr(kPlanVersionAttr, StringAttr::get(context, kPlanVersion));
  comb->setAttr(kWorkAttr, integer(work));
  comb->setAttr(kMaxNodesAttr, integer(maxNodes));
}

static LogicalResult materializePartitions(pyc::CombOp original,
                                           ArrayRef<Operation *> order,
                                           ArrayRef<unsigned> ends,
                                           uint64_t parentId,
                                           uint64_t maxNodes) {
  if (ends.size() == 1) {
    stampPartition(original, parentId, 0, 1, order.size(), maxNodes);
    return success();
  }

  Block &originalBody = original.getBody().front();
  auto originalYield = cast<pyc::YieldOp>(originalBody.getTerminator());

  // Maps values in the original isolated region to values visible at the
  // sibling-comb level.  Block arguments start as the original comb operands;
  // each partition adds its live-outs for subsequent partitions.
  IRMapping outerMapping;
  for (auto [argument, input] :
       llvm::zip(originalBody.getArguments(), original.getInputs()))
    outerMapping.map(argument, input);

  unsigned begin = 0;
  for (auto [partId, end] : llvm::enumerate(ends)) {
    llvm::DenseSet<Operation *> interval;
    for (unsigned i = begin; i < end; ++i)
      interval.insert(order[i]);

    llvm::SmallVector<Value> originalInputs;
    llvm::DenseSet<Value> seenInputs;
    for (unsigned i = begin; i < end; ++i) {
      for (Value operand : order[i]->getOperands()) {
        if (interval.contains(operand.getDefiningOp()))
          continue;
        if (seenInputs.insert(operand).second)
          originalInputs.push_back(operand);
      }
    }

    llvm::SmallVector<Value> originalOutputs;
    for (unsigned i = begin; i < end; ++i) {
      for (Value result : order[i]->getResults()) {
        bool liveOut = llvm::any_of(result.getUses(), [&](OpOperand &use) {
          return !interval.contains(use.getOwner());
        });
        if (liveOut)
          originalOutputs.push_back(result);
      }
    }

    llvm::SmallVector<Value> inputs;
    inputs.reserve(originalInputs.size());
    for (Value input : originalInputs) {
      Value mapped = outerMapping.lookupOrNull(input);
      if (!mapped) {
        original.emitOpError("partition input is not available from an earlier "
                             "topological part");
        return failure();
      }
      inputs.push_back(mapped);
    }

    llvm::SmallVector<Type> resultTypes;
    for (Value output : originalOutputs)
      resultTypes.push_back(output.getType());

    OpBuilder builder(original);
    auto part =
        builder.create<pyc::CombOp>(original.getLoc(), resultTypes, inputs);
    stampPartition(part, parentId, partId, ends.size(), end - begin, maxNodes);

    Block *body = new Block();
    part.getBody().push_back(body);
    for (Value input : inputs)
      body->addArgument(input.getType(), original.getLoc());

    IRMapping localMapping;
    for (auto [input, argument] :
         llvm::zip(originalInputs, body->getArguments()))
      localMapping.map(input, argument);

    builder.setInsertionPointToStart(body);
    for (unsigned i = begin; i < end; ++i) {
      Operation *cloned = builder.clone(*order[i], localMapping);
      for (auto [oldResult, newResult] :
           llvm::zip(order[i]->getResults(), cloned->getResults()))
        localMapping.map(oldResult, newResult);
    }

    llvm::SmallVector<Value> yielded;
    for (Value output : originalOutputs)
      yielded.push_back(localMapping.lookup(output));
    builder.create<pyc::YieldOp>(original.getLoc(), yielded);

    for (auto [output, result] : llvm::zip(originalOutputs, part.getResults()))
      outerMapping.map(output, result);
    begin = end;
  }

  for (auto [oldResult, yielded] :
       llvm::zip(original.getResults(), originalYield.getValues())) {
    Value replacement = outerMapping.lookupOrNull(yielded);
    if (!replacement) {
      original.emitOpError("partition plan did not publish an original output");
      return failure();
    }
    oldResult.replaceAllUsesWith(replacement);
  }
  original.erase();
  return success();
}

struct PartitionCombPass
    : public PassWrapper<PartitionCombPass, OperationPass<func::FuncOp>> {
  MLIR_DEFINE_EXPLICIT_INTERNAL_INLINE_TYPE_ID(PartitionCombPass)

  Option<unsigned> maxNodes{
      *this, "max-nodes",
      llvm::cl::desc("Maximum operation count in a runtime pyc.comb partition"),
      llvm::cl::init(35)};

  PartitionCombPass() = default;
  PartitionCombPass(const PartitionCombPass &other) : PassWrapper(other) {}
  explicit PartitionCombPass(unsigned requestedMaxNodes) {
    maxNodes = requestedMaxNodes;
  }

  StringRef getArgument() const override { return "pyc-partition-comb"; }
  StringRef getDescription() const override {
    return "Partition pyc.comb into GSIM-style contiguous-topological runtime "
           "units";
  }

  void runOnOperation() override {
    if (maxNodes == 0)
      return;

    func::FuncOp function = getOperation();
    llvm::SmallVector<pyc::CombOp> candidates;
    function.walk([&](pyc::CombOp comb) {
      if (!hasAnyPartitionAttribute(comb))
        candidates.push_back(comb);
    });

    uint64_t nextParentId = 0;
    function.walk([&](pyc::CombOp comb) {
      if (auto id = comb->getAttrOfType<IntegerAttr>(kParentIdAttr)) {
        if (id.getInt() >= 0)
          nextParentId =
              std::max(nextParentId, static_cast<uint64_t>(id.getInt()) + 1);
      }
    });

    for (pyc::CombOp comb : candidates) {
      auto order = stableTopologicalOrder(comb);
      if (failed(order)) {
        signalPassFailure();
        return;
      }
      auto coarsened = coarsenOperations(*order, maxNodes);
      if (failed(coarsened)) {
        comb.emitOpError(
            "GSIM-style coarsening produced a cyclic quotient graph");
        signalPassFailure();
        return;
      }
      auto ends =
          choosePartEnds(coarsened->operations, coarsened->atomEnds, maxNodes);
      if (failed(ends)) {
        comb.emitOpError("failed to find a legal contiguous partition plan");
        signalPassFailure();
        return;
      }
      if (failed(materializePartitions(comb, coarsened->operations, *ends,
                                       nextParentId++, maxNodes))) {
        signalPassFailure();
        return;
      }
    }
  }
};

} // namespace

std::unique_ptr<mlir::Pass> createPartitionCombPass(unsigned maxNodes) {
  return std::make_unique<PartitionCombPass>(maxNodes);
}

static PassRegistration<PartitionCombPass> pass;

} // namespace pyc
