#include "pyc/Transforms/FusedCombPartition.h"

#include "pyc/Dialect/PYC/PYCOps.h"
#include "pyc/Transforms/CombMemoization.h"

#include "mlir/IR/Block.h"
#include "mlir/IR/BuiltinTypes.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/STLExtras.h"

#include <algorithm>
#include <cstdint>
#include <functional>
#include <limits>
#include <map>
#include <queue>
#include <utility>
#include <vector>

using namespace mlir;

namespace pyc {
namespace {

static LogicalResult verifyMemoizableBodyOperation(Operation &op) {
  if (!isMemoizableCombOperation(&op))
    return op.emitError(
        "operation is not supported by local pyc.comb partitioning");
  for (Type type : op.getOperandTypes())
    if (!isMemoizableCombType(type))
      return op.emitError("operand type is not exactly memoizable: ") << type;
  for (Type type : op.getResultTypes())
    if (!isMemoizableCombType(type))
      return op.emitError("result type is not exactly memoizable: ") << type;
  return success();
}

class CoarsenedGroups {
public:
  explicit CoarsenedGroups(unsigned count)
      : parent_(count), work_(count, 1), firstMember_(count) {
    for (unsigned i = 0; i < count; ++i)
      parent_[i] = firstMember_[i] = i;
  }

  unsigned find(unsigned node) {
    if (parent_[node] != node)
      parent_[node] = find(parent_[node]);
    return parent_[node];
  }

  void mergeInto(unsigned source, unsigned target) {
    source = find(source);
    target = find(target);
    if (source == target)
      return;
    parent_[source] = target;
    work_[target] += work_[source];
    firstMember_[target] = std::min(firstMember_[target], firstMember_[source]);
  }

  unsigned getWork(unsigned node) { return work_[find(node)]; }
  unsigned getFirstMember(unsigned node) { return firstMember_[find(node)]; }

private:
  std::vector<unsigned> parent_;
  std::vector<unsigned> work_;
  std::vector<unsigned> firstMember_;
};

struct QuotientGraph {
  llvm::SmallVector<unsigned> topologicalRoots;
  std::vector<llvm::DenseSet<unsigned>> predecessors;
  std::vector<llvm::DenseSet<unsigned>> successors;
};

static FailureOr<QuotientGraph>
buildQuotientGraph(const FusedCombDepGraph &graph,
                   ArrayRef<unsigned> initialOrder, CoarsenedGroups &groups) {
  const unsigned nodeCount = initialOrder.size();
  QuotientGraph quotient;
  quotient.predecessors.resize(nodeCount);
  quotient.successors.resize(nodeCount);

  std::vector<unsigned> orderPosition(nodeCount);
  for (auto [position, node] : llvm::enumerate(initialOrder))
    orderPosition[node] = static_cast<unsigned>(position);

  for (unsigned sourceNode = 0; sourceNode < nodeCount; ++sourceNode) {
    unsigned source = groups.find(orderPosition[sourceNode]);
    for (unsigned targetNode : graph.getSuccessors(sourceNode)) {
      unsigned target = groups.find(orderPosition[targetNode]);
      if (source == target)
        continue;
      quotient.successors[source].insert(target);
      quotient.predecessors[target].insert(source);
    }
  }

  using Ready = std::pair<unsigned, unsigned>;
  std::priority_queue<Ready, std::vector<Ready>, std::greater<Ready>> ready;
  std::vector<unsigned> indegree(nodeCount);
  unsigned rootCount = 0;
  for (unsigned i = 0; i < nodeCount; ++i) {
    if (groups.find(i) != i)
      continue;
    ++rootCount;
    indegree[i] = quotient.predecessors[i].size();
    if (indegree[i] == 0)
      ready.emplace(groups.getFirstMember(i), i);
  }

  while (!ready.empty()) {
    unsigned root = ready.top().second;
    ready.pop();
    quotient.topologicalRoots.push_back(root);
    llvm::SmallVector<unsigned> successors(quotient.successors[root].begin(),
                                           quotient.successors[root].end());
    llvm::sort(successors, [&](unsigned lhs, unsigned rhs) {
      return groups.getFirstMember(lhs) < groups.getFirstMember(rhs);
    });
    for (unsigned successor : successors)
      if (--indegree[successor] == 0)
        ready.emplace(groups.getFirstMember(successor), successor);
  }

  if (quotient.topologicalRoots.size() != rootCount)
    return failure();
  return quotient;
}

static bool hasPath(unsigned source, unsigned target,
                    const QuotientGraph &graph) {
  if (source == target)
    return true;
  llvm::SmallVector<unsigned> worklist{source};
  llvm::DenseSet<unsigned> visited{source};
  while (!worklist.empty()) {
    unsigned current = worklist.pop_back_val();
    for (unsigned successor : graph.successors[current]) {
      if (successor == target)
        return true;
      if (visited.insert(successor).second)
        worklist.push_back(successor);
    }
  }
  return false;
}

struct CoarsenedOrder {
  llvm::SmallVector<unsigned> nodes;
  llvm::SmallVector<unsigned> atomEnds;
};

static FailureOr<CoarsenedOrder> coarsen(const FusedCombDepGraph &graph,
                                         ArrayRef<unsigned> initialOrder,
                                         unsigned maxNodes) {
  CoarsenedGroups groups(initialOrder.size());

  // Rebuild after every merge. This is slightly more work than updating local
  // degrees, but makes each decision against the actual quotient DAG.
  bool changed = true;
  while (changed) {
    changed = false;
    auto quotient = buildQuotientGraph(graph, initialOrder, groups);
    if (failed(quotient))
      return failure();
    for (unsigned root : llvm::reverse(quotient->topologicalRoots)) {
      if (quotient->successors[root].size() != 1)
        continue;
      unsigned target = *quotient->successors[root].begin();
      if (groups.getWork(root) + groups.getWork(target) > maxNodes)
        continue;
      groups.mergeInto(root, target); // mergeOut1
      changed = true;
      break;
    }
  }

  changed = true;
  while (changed) {
    changed = false;
    auto quotient = buildQuotientGraph(graph, initialOrder, groups);
    if (failed(quotient))
      return failure();
    for (unsigned root : quotient->topologicalRoots) {
      if (quotient->predecessors[root].size() != 1)
        continue;
      unsigned source = *quotient->predecessors[root].begin();
      if (groups.getWork(source) + groups.getWork(root) > maxNodes)
        continue;
      groups.mergeInto(root, source); // mergeIn1
      changed = true;
      break;
    }
  }

  changed = true;
  while (changed) {
    changed = false;
    auto quotient = buildQuotientGraph(graph, initialOrder, groups);
    if (failed(quotient))
      return failure();
    std::map<std::vector<unsigned>, unsigned> representative;
    for (unsigned root : quotient->topologicalRoots) {
      if (quotient->predecessors[root].empty())
        continue;
      std::vector<unsigned> key(quotient->predecessors[root].begin(),
                                quotient->predecessors[root].end());
      llvm::sort(key, [&](unsigned lhs, unsigned rhs) {
        return groups.getFirstMember(lhs) < groups.getFirstMember(rhs);
      });
      auto [it, inserted] = representative.emplace(std::move(key), root);
      if (inserted)
        continue;
      unsigned sibling = groups.find(it->second);
      root = groups.find(root);
      if (sibling == root ||
          groups.getWork(sibling) + groups.getWork(root) > maxNodes ||
          hasPath(sibling, root, *quotient) ||
          hasPath(root, sibling, *quotient))
        continue;
      groups.mergeInto(root, sibling); // mergeSiblings
      changed = true;
      break;
    }
  }

  auto quotient = buildQuotientGraph(graph, initialOrder, groups);
  if (failed(quotient))
    return failure();
  std::vector<llvm::SmallVector<unsigned>> members(initialOrder.size());
  for (unsigned position = 0; position < initialOrder.size(); ++position)
    members[groups.find(position)].push_back(initialOrder[position]);

  CoarsenedOrder result;
  for (unsigned root : quotient->topologicalRoots) {
    result.nodes.append(members[root]);
    result.atomEnds.push_back(result.nodes.size());
  }
  return result;
}

static uint64_t intervalCutCost(const FusedCombDepGraph &graph,
                                ArrayRef<unsigned> order,
                                ArrayRef<unsigned> position, unsigned begin,
                                unsigned end) {
  uint64_t cost = 0;
  for (unsigned offset = begin; offset < end; ++offset)
    for (unsigned successor : graph.getSuccessors(order[offset]))
      if (position[successor] < begin || position[successor] >= end)
        ++cost;
  return cost;
}

static FailureOr<llvm::SmallVector<unsigned>>
chooseEnds(const FusedCombDepGraph &graph, ArrayRef<unsigned> order,
           ArrayRef<unsigned> atomEnds, unsigned maxNodes) {
  if (order.empty())
    return llvm::SmallVector<unsigned>{0};
  if (atomEnds.empty() || atomEnds.back() != order.size())
    return failure();

  std::vector<unsigned> position(order.size());
  for (auto [offset, node] : llvm::enumerate(order))
    position[node] = static_cast<unsigned>(offset);

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
      uint64_t candidateCost =
          bestCost[beginAtom] +
          intervalCutCost(graph, order, position, begin, end);
      unsigned candidateParts = bestParts[beginAtom] + 1;
      bool better = candidateCost < bestCost[endAtom];
      if (candidateCost == bestCost[endAtom])
        better = candidateParts < bestParts[endAtom] ||
                 (candidateParts == bestParts[endAtom] &&
                  beginAtom < predecessor[endAtom]);
      if (!better)
        continue;
      bestCost[endAtom] = candidateCost;
      bestParts[endAtom] = candidateParts;
      predecessor[endAtom] = beginAtom;
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

} // namespace

FailureOr<std::unique_ptr<FusedCombDepGraph>>
FusedCombDepGraph::build(CombOp comb) {
  if (comb.getBody().empty() || !llvm::hasSingleElement(comb.getBody())) {
    comb.emitOpError("local partitioning requires a single-block body");
    return failure();
  }
  Block &body = comb.getBody().front();
  if (body.getNumArguments() != comb.getNumOperands()) {
    comb.emitOpError("body argument count must match comb input count");
    return failure();
  }
  for (auto [argument, input] :
       llvm::zip(body.getArguments(), comb.getInputs())) {
    if (argument.getType() != input.getType() ||
        !isMemoizableCombType(argument.getType())) {
      comb.emitOpError(
          "body arguments must match exactly memoizable comb inputs");
      return failure();
    }
  }

  auto yield = dyn_cast_or_null<YieldOp>(body.getTerminator());
  if (!yield || yield.getNumOperands() != comb.getNumResults()) {
    comb.emitOpError("local partitioning requires a matching pyc.yield");
    return failure();
  }
  for (auto [value, result] : llvm::zip(yield.getValues(), comb.getResults())) {
    if (value.getType() != result.getType() ||
        !isMemoizableCombType(result.getType())) {
      comb.emitOpError(
          "pyc.yield values must match exactly memoizable comb results");
      return failure();
    }
  }

  auto graph = std::unique_ptr<FusedCombDepGraph>(new FusedCombDepGraph());
  llvm::DenseMap<Operation *, unsigned> operationIndex;
  for (Operation &op : body.without_terminator()) {
    if (failed(verifyMemoizableBodyOperation(op)))
      return failure();
    operationIndex[&op] = graph->operations_.size();
    graph->operations_.push_back(&op);
  }
  graph->predecessors_.resize(graph->operations_.size());
  graph->successors_.resize(graph->operations_.size());

  for (auto [targetIndex, op] : llvm::enumerate(graph->operations_)) {
    llvm::DenseSet<unsigned> seenPredecessors;
    for (Value operand : op->getOperands()) {
      if (auto argument = dyn_cast<BlockArgument>(operand)) {
        if (argument.getOwner() != &body) {
          op->emitError("operand captures a value from outside pyc.comb");
          return failure();
        }
        continue;
      }
      auto source = operationIndex.find(operand.getDefiningOp());
      if (source == operationIndex.end()) {
        op->emitError("operand is not defined by the pyc.comb body");
        return failure();
      }
      if (!seenPredecessors.insert(source->second).second)
        continue;
      graph->predecessors_[targetIndex].push_back(source->second);
      graph->successors_[source->second].push_back(targetIndex);
    }
  }

  for (Value value : yield.getValues()) {
    if (auto argument = dyn_cast<BlockArgument>(value)) {
      if (argument.getOwner() == &body)
        continue;
    } else if (operationIndex.count(value.getDefiningOp())) {
      continue;
    }
    yield.emitOpError("operand is not defined by the pyc.comb body");
    return failure();
  }
  for (auto &nodes : graph->predecessors_)
    llvm::sort(nodes);
  for (auto &nodes : graph->successors_)
    llvm::sort(nodes);

  if (failed(graph->stableTopologicalOrder())) {
    comb.emitOpError("body dependency graph contains a cycle");
    return failure();
  }
  return graph;
}

FailureOr<llvm::SmallVector<unsigned>>
FusedCombDepGraph::stableTopologicalOrder() const {
  std::vector<unsigned> indegree(operations_.size());
  std::priority_queue<unsigned, std::vector<unsigned>, std::greater<unsigned>>
      ready;
  for (unsigned node = 0; node < operations_.size(); ++node) {
    indegree[node] = predecessors_[node].size();
    if (indegree[node] == 0)
      ready.push(node);
  }

  llvm::SmallVector<unsigned> order;
  while (!ready.empty()) {
    unsigned node = ready.top();
    ready.pop();
    order.push_back(node);
    for (unsigned successor : successors_[node])
      if (--indegree[successor] == 0)
        ready.push(successor);
  }
  if (order.size() != operations_.size())
    return failure();
  return order;
}

FailureOr<FusedCombPartitionPlan>
planFusedCombPartitions(const FusedCombDepGraph &graph, unsigned maxNodes) {
  if (maxNodes == 0)
    return failure();
  auto initialOrder = graph.stableTopologicalOrder();
  if (failed(initialOrder))
    return failure();
  auto coarsened = coarsen(graph, *initialOrder, maxNodes);
  if (failed(coarsened))
    return failure();
  auto ends =
      chooseEnds(graph, coarsened->nodes, coarsened->atomEnds, maxNodes);
  if (failed(ends))
    return failure();
  return FusedCombPartitionPlan{std::move(coarsened->nodes), std::move(*ends)};
}

} // namespace pyc
