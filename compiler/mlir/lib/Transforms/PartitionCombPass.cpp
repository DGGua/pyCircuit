#include "pyc/Transforms/Passes.h"

#include "pyc/Dialect/PYC/PYCOps.h"
#include "pyc/Transforms/CombDepGraph.h"
#include "pyc/Transforms/CombMemoization.h"
#include "pyc/Transforms/CombPartition.h"

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

/// Operation-level view derived from the canonical value graph.  All GSIM
/// coarsening and DP decisions consume this projection; no partition stage is
/// allowed to invent a second, subtly different dependence graph.
struct CandidateDepProjection {
  llvm::SmallVector<Operation *> operations;
  llvm::DenseMap<Operation *, unsigned> index;
  std::vector<llvm::DenseSet<unsigned>> predecessors;
  std::vector<llvm::DenseSet<unsigned>> successors;
};

// Project paths through non-materializable value nodes (wire/assign,
// primitive and instance boundaries) onto the next materializable operation.
// This is deliberately function-level: a barrier constrains placement but
// never erases a same-TICK semantic dependency.
static CandidateDepProjection
buildCandidateProjection(ArrayRef<Operation *> operations,
                         const FunctionCombDepGraph &graph) {
  CandidateDepProjection projection;
  projection.operations.assign(operations.begin(), operations.end());
  for (auto [i, op] : llvm::enumerate(operations))
    projection.index[op] = static_cast<unsigned>(i);

  projection.predecessors.resize(operations.size());
  projection.successors.resize(operations.size());
  ArrayRef<CombDepValueNode> nodes = graph.getNodes();
  ArrayRef<CombDepEdge> edges = graph.getEdges();
  for (auto [sourceIndex, sourceOperation] : llvm::enumerate(operations)) {
    llvm::SmallVector<unsigned> worklist;
    llvm::DenseSet<unsigned> visited;
    for (unsigned nodeId = 0; nodeId < nodes.size(); ++nodeId) {
      if (nodes[nodeId].producer != sourceOperation)
        continue;
      worklist.push_back(nodeId);
      visited.insert(nodeId);
    }
    while (!worklist.empty()) {
      unsigned nodeId = worklist.pop_back_val();
      for (unsigned edgeId : nodes[nodeId].outgoingEdges) {
        unsigned targetNode = edges[edgeId].target;
        if (!visited.insert(targetNode).second)
          continue;
        Operation *targetOperation = nodes[targetNode].producer;
        auto target = projection.index.find(targetOperation);
        if (target != projection.index.end() &&
            target->second != sourceIndex) {
          projection.successors[sourceIndex].insert(target->second);
          projection.predecessors[target->second].insert(
              static_cast<unsigned>(sourceIndex));
          continue;
        }
        worklist.push_back(targetNode);
      }
    }
  }
  return projection;
}

static FailureOr<llvm::SmallVector<Operation *>>
stableTopologicalOrder(const CandidateDepProjection &projection) {
  const unsigned operationCount = projection.operations.size();
  llvm::SmallVector<llvm::SmallVector<unsigned>> successors(operationCount);
  llvm::SmallVector<unsigned> indegree(operationCount, 0);
  for (unsigned i = 0; i < operationCount; ++i) {
    indegree[i] = projection.predecessors[i].size();
    successors[i].append(projection.successors[i].begin(),
                         projection.successors[i].end());
  }
  for (auto &list : successors)
    llvm::sort(list);

  std::priority_queue<unsigned, std::vector<unsigned>, std::greater<unsigned>>
      ready;
  for (unsigned i = 0; i < operationCount; ++i)
    if (indegree[i] == 0)
      ready.push(i);

  llvm::SmallVector<Operation *> ordered;
  ordered.reserve(operationCount);
  while (!ready.empty()) {
    unsigned current = ready.top();
    ready.pop();
    ordered.push_back(projection.operations[current]);
    for (unsigned successor : successors[current])
      if (--indegree[successor] == 0)
        ready.push(successor);
  }

  if (ordered.size() != operationCount) {
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
buildCoarsenedGraph(ArrayRef<Operation *> order, CoarsenedGroups &groups,
                    const CandidateDepProjection &projection) {
  const unsigned n = order.size();
  CoarsenedGraph graph;
  graph.predecessors.resize(n);
  graph.successors.resize(n);

  llvm::DenseMap<Operation *, unsigned> position;
  for (auto [i, op] : llvm::enumerate(order))
    position[op] = static_cast<unsigned>(i);

  for (auto [i, op] : llvm::enumerate(order)) {
    unsigned source = groups.find(static_cast<unsigned>(i));
    auto projectedSource = projection.index.find(op);
    if (projectedSource == projection.index.end())
      return failure();
    for (unsigned projectedTarget :
         projection.successors[projectedSource->second]) {
      Operation *targetOperation = projection.operations[projectedTarget];
      auto it = position.find(targetOperation);
      if (it == position.end())
        continue;
      unsigned target = groups.find(it->second);
      if (source == target)
        continue;
      graph.successors[source].insert(target);
      graph.predecessors[target].insert(source);
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
coarsenOperations(ArrayRef<Operation *> initialOrder, unsigned maxNodes,
                  const CandidateDepProjection &projection) {
  CoarsenedGroups groups(initialOrder.size());
  if (initialOrder.empty())
    return CoarsenedOrder{};

  // mergeOut1: reverse topological order, folding a single-fanout producer into
  // its consumer when the combined atom stays within the runtime work cap.
  auto graphOr = buildCoarsenedGraph(initialOrder, groups, projection);
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
  graphOr = buildCoarsenedGraph(initialOrder, groups, projection);
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
  graphOr = buildCoarsenedGraph(initialOrder, groups, projection);
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

  graphOr = buildCoarsenedGraph(initialOrder, groups, projection);
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
                     unsigned begin, unsigned end,
                     const CandidateDepProjection &projection) {
  uint64_t cost = 0;
  for (unsigned i = begin; i < end; ++i) {
    Operation *sourceOperation = order[i];
    auto source = projection.index.find(sourceOperation);
    if (source != projection.index.end()) {
      for (unsigned target : projection.successors[source->second]) {
        auto targetPosition =
            position.find(projection.operations[target]);
        if (targetPosition == position.end() || targetPosition->second < begin ||
            targetPosition->second >= end)
          ++cost;
      }
    }
    for (Value result : sourceOperation->getResults()) {
      llvm::DenseSet<Operation *> outsideUsers;
      for (OpOperand &use : result.getUses()) {
        Operation *owner = use.getOwner();
        // Candidate-to-candidate dependencies were counted from the canonical
        // projection above, including paths through placement barriers.
        if (projection.index.count(owner))
          continue;
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
               unsigned maxNodes,
               const CandidateDepProjection &projection) {
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
      uint64_t boundary =
          intervalBoundaryCost(order, position, begin, end, projection);
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
  comb->setAttr(kCombPartitionParentIdAttr, integer(parentId));
  comb->setAttr(kCombPartitionPartIdAttr, integer(partId));
  comb->setAttr(kCombPartitionPartCountAttr, integer(partCount));
  comb->setAttr(kCombPartitionPlanVersionAttr,
                StringAttr::get(context, kCombPartitionPlanVersion));
  comb->setAttr(kCombPartitionWorkAttr, integer(work));
  comb->setAttr(kCombPartitionMaxNodesAttr, integer(maxNodes));
}

struct StagePlan {
  llvm::SmallVector<Operation *> order;
  llvm::SmallVector<unsigned> ends;
  Operation *anchor = nullptr;
  uint64_t parentId = 0;
};

struct FunctionPartitionPlan {
  func::FuncOp function;
  llvm::SmallVector<Operation *> candidates;
  llvm::SmallVector<StagePlan> stages;
};

static bool isStructuralFunction(func::FuncOp function) {
  auto structural =
      function->getAttrOfType<StringAttr>("pyc.emit.structural");
  if (!structural)
    return false;
  StringRef value = structural.getValue();
  return value.equals_insensitive("true") || value == "1";
}

static LogicalResult verifyMemoizableOperation(Operation *op) {
  if (!isMemoizableCombOperation(op)) {
    op->emitError("operation is not in the deterministic pyc.comb contract");
    return failure();
  }
  for (Type type : op->getOperandTypes()) {
    if (isMemoizableCombType(type))
      continue;
    op->emitError("operand type cannot be compared exactly by pyc.comb: ")
        << type;
    return failure();
  }
  for (Type type : op->getResultTypes()) {
    if (isMemoizableCombType(type))
      continue;
    op->emitError("result type cannot be compared exactly by pyc.comb: ")
        << type;
    return failure();
  }
  return success();
}

// Validate and transparently unfold all existing comb regions.  This is done
// inside the unified pass, rather than running FuseComb followed by a second
// partitioning pass, so old region/run boundaries cannot bias the final plan.
static LogicalResult unfoldExistingCombs(func::FuncOp function) {
  Block &block = function.getBody().front();
  llvm::SmallVector<pyc::CombOp> combs;
  for (Operation &op : block)
    if (auto comb = dyn_cast<pyc::CombOp>(op))
      combs.push_back(comb);

  for (pyc::CombOp comb : combs) {
    if (comb.getBody().empty() || !llvm::hasSingleElement(comb.getBody())) {
      comb.emitOpError("unified partitioning requires a single-block body");
      return failure();
    }
    Block &body = comb.getBody().front();
    auto yield = dyn_cast_or_null<pyc::YieldOp>(body.getTerminator());
    if (!yield || yield.getNumOperands() != comb.getNumResults()) {
      comb.emitOpError("unified partitioning requires a matching pyc.yield");
      return failure();
    }
    for (Operation &nested : body.without_terminator())
      if (failed(verifyMemoizableOperation(&nested)))
        return failure();

    OpBuilder builder(comb);
    IRMapping mapping;
    for (auto [argument, input] : llvm::zip(body.getArguments(),
                                            comb.getInputs()))
      mapping.map(argument, input);
    for (Operation &nested : body.without_terminator()) {
      Operation *clone = builder.clone(nested, mapping);
      for (auto [oldResult, newResult] :
           llvm::zip(nested.getResults(), clone->getResults()))
        mapping.map(oldResult, newResult);
    }
    ArrayAttr resultNames =
        comb->getAttrOfType<ArrayAttr>(kCombResultNamesAttr);
    if (resultNames && resultNames.size() != comb.getNumResults()) {
      comb.emitOpError("'")
          << kCombResultNamesAttr << "' length must match result arity";
      return failure();
    }
    StringAttr singleWrapperName =
        comb.getNumResults() == 1
            ? comb->getAttrOfType<StringAttr>("pyc.name")
            : StringAttr();
    llvm::SmallVector<Value> replacements;
    llvm::SmallVector<StringAttr> promotedNames;
    replacements.reserve(comb.getNumResults());
    promotedNames.reserve(comb.getNumResults());
    for (auto [index, pair] :
         llvm::enumerate(llvm::zip(comb.getResults(), yield.getValues()))) {
      auto [unusedResult, yielded] = pair;
      (void)unusedResult;
      Value replacement = mapping.lookupOrNull(yielded);
      if (!replacement) {
        comb.emitOpError("failed to map an unfolded pyc.comb result");
        return failure();
      }
      StringAttr promotedName;
      if (resultNames && index < resultNames.size())
        promotedName = dyn_cast<StringAttr>(resultNames[index]);
      if ((!promotedName || promotedName.getValue().empty()) && index == 0)
        promotedName = singleWrapperName;
      replacements.push_back(replacement);
      promotedNames.push_back(promotedName);
    }

    for (auto [result, replacement, promotedName] :
         llvm::zip(comb.getResults(), replacements, promotedNames)) {
      if (promotedName && !promotedName.getValue().empty()) {
        Operation *definition = replacement.getDefiningOp();
        StringAttr existingName =
            definition && definition->getNumResults() == 1
                ? definition->getAttrOfType<StringAttr>("pyc.name")
                : StringAttr();
        if (!definition || definition->getNumResults() != 1 ||
            (existingName && existingName != promotedName)) {
          // A wrapper result is an independently observable SSA value.  If
          // two named results yield the same body value, the first name stays
          // on that definition and later distinct names get aliases.  If the
          // body already carries a different name, retain both identities
          // instead of overwriting one with another.  On a subsequent
          // replan these aliases already have matching one-to-one names, so
          // the rewrite is stable rather than growing an alias chain.
          auto alias = builder.create<pyc::AliasOp>(
              comb.getLoc(), replacement.getType(), replacement);
          alias->setAttr("pyc.name", promotedName);
          replacement = alias.getResult();
        } else if (!existingName) {
          definition->setAttr("pyc.name", promotedName);
        }
      }
      result.replaceAllUsesWith(replacement);
    }
    comb.erase();
  }
  return success();
}

static Operation *topLevelOwner(Operation *operation, Block *block) {
  while (operation && operation->getBlock() != block)
    operation = operation->getParentOp();
  return operation;
}

// Return the earliest legal insertion anchor for a group.  A pure group can
// move after all external definitions and before all external uses.  Empty
// intervals expose state/instance/wire barriers that require a separate
// parent plan.
static FailureOr<Operation *>
findPlacementAnchor(ArrayRef<Operation *> operations, Block &block,
                    const llvm::DenseMap<Operation *, unsigned> &position) {
  llvm::DenseSet<Operation *> group(operations.begin(), operations.end());
  int64_t latestDefinition = -1;
  uint64_t earliestUse = std::numeric_limits<uint64_t>::max();

  for (Operation *op : operations) {
    for (Value input : op->getOperands()) {
      Operation *definition = topLevelOwner(input.getDefiningOp(), &block);
      if (!definition || group.contains(definition))
        continue;
      auto found = position.find(definition);
      if (found != position.end())
        latestDefinition = std::max<int64_t>(latestDefinition, found->second);
      // pyc.wire is declared at one position but obtains its same-TICK value
      // from pyc.assign drivers elsewhere in the block.  Treat the latest
      // driver as the real availability boundary; otherwise a producer and a
      // wire consumer could be folded into one Comb around the AssignOp and
      // force a backend fixed-point fallback.
      if (isa<pyc::WireOp>(definition)) {
        for (Operation &candidateDriver : block) {
          auto assign = dyn_cast<pyc::AssignOp>(candidateDriver);
          if (!assign || assign.getDst() != input)
            continue;
          auto driverPosition = position.find(&candidateDriver);
          if (driverPosition != position.end())
            latestDefinition = std::max<int64_t>(latestDefinition,
                                                 driverPosition->second);
        }
      }
    }
    for (Value output : op->getResults()) {
      // A named value is externally observable through the probe/public-value
      // contract even when it has no SSA user.  Model that observation as a
      // terminator endpoint so DCE/placement cannot silently localize it.
      if (op->getNumResults() == 1) {
        if (auto name = op->getAttrOfType<StringAttr>("pyc.name");
            name && !name.getValue().empty()) {
          auto terminator = position.find(block.getTerminator());
          if (terminator != position.end())
            earliestUse =
                std::min<uint64_t>(earliestUse, terminator->second);
        }
      }
      for (OpOperand &use : output.getUses()) {
        Operation *owner = topLevelOwner(use.getOwner(), &block);
        if (!owner || group.contains(owner))
          continue;
        auto found = position.find(owner);
        if (found != position.end())
          earliestUse = std::min<uint64_t>(earliestUse, found->second);
      }
    }
  }

  if (earliestUse == std::numeric_limits<uint64_t>::max())
    return static_cast<Operation *>(nullptr);
  if (latestDefinition >= static_cast<int64_t>(earliestUse))
    return failure();
  for (Operation &op : block)
    if (position.lookup(&op) == earliestUse)
      return &op;
  return failure();
}

static FailureOr<llvm::SmallVector<StagePlan>>
planStages(ArrayRef<Operation *> topologicalOrder, Block &block,
           unsigned maxNodes, const CandidateDepProjection &projection) {
  llvm::DenseMap<Operation *, unsigned> position;
  for (auto [index, op] : llvm::enumerate(block))
    position[&op] = static_cast<unsigned>(index);

  llvm::SmallVector<llvm::SmallVector<Operation *>> rawStages;
  llvm::SmallVector<Operation *> current;
  for (Operation *op : topologicalOrder) {
    current.push_back(op);
    if (succeeded(findPlacementAnchor(current, block, position)))
      continue;
    current.pop_back();
    if (current.empty()) {
      op->emitError("memoizable operation has no legal materialization point");
      return failure();
    }
    rawStages.push_back(current);
    current.clear();
    current.push_back(op);
    if (failed(findPlacementAnchor(current, block, position))) {
      op->emitError("memoizable operation crosses an unsplittable barrier");
      return failure();
    }
  }
  if (!current.empty())
    rawStages.push_back(current);

  llvm::SmallVector<StagePlan> plans;
  uint64_t parentId = 0;
  for (ArrayRef<Operation *> stage : rawStages) {
    auto anchor = findPlacementAnchor(stage, block, position);
    if (failed(anchor))
      return failure();
    // A dead pure component should have been removed by DCE.  Keeping it out
    // of the runtime plan is nevertheless safe and avoids a zero-result call.
    if (*anchor == nullptr)
      continue;
    auto coarsened = coarsenOperations(stage, maxNodes, projection);
    if (failed(coarsened))
      return failure();
    auto ends = choosePartEnds(coarsened->operations, coarsened->atomEnds,
                               maxNodes, projection);
    if (failed(ends))
      return failure();
    plans.push_back(StagePlan{std::move(coarsened->operations),
                              std::move(*ends), *anchor, parentId++});
  }
  return plans;
}

static LogicalResult
materializeStage(const StagePlan &plan, uint64_t maxNodes,
                 const llvm::DenseSet<Operation *> &allCandidates,
                 IRMapping &outerMapping) {
  unsigned begin = 0;
  for (auto [partId, end] : llvm::enumerate(plan.ends)) {
    llvm::DenseSet<Operation *> interval;
    for (unsigned i = begin; i < end; ++i)
      interval.insert(plan.order[i]);

    llvm::SmallVector<Value> originalInputs;
    llvm::DenseSet<Value> seenInputs;
    for (unsigned i = begin; i < end; ++i) {
      for (Value operand : plan.order[i]->getOperands()) {
        if (interval.contains(operand.getDefiningOp()))
          continue;
        if (seenInputs.insert(operand).second)
          originalInputs.push_back(operand);
      }
    }

    llvm::SmallVector<Value> originalOutputs;
    for (unsigned i = begin; i < end; ++i) {
      for (Value result : plan.order[i]->getResults()) {
        StringAttr observableName;
        if (Operation *definition = result.getDefiningOp();
            definition && definition->getNumResults() == 1)
          observableName =
              definition->getAttrOfType<StringAttr>("pyc.name");
        bool liveOut = llvm::any_of(result.getUses(), [&](OpOperand &use) {
          return !interval.contains(use.getOwner());
        });
        if (liveOut || (observableName && !observableName.getValue().empty()))
          originalOutputs.push_back(result);
      }
    }

    llvm::SmallVector<Value> inputs;
    inputs.reserve(originalInputs.size());
    for (Value input : originalInputs) {
      Value mapped = outerMapping.lookupOrNull(input);
      if (!mapped) {
        Operation *definition = input.getDefiningOp();
        if (definition && allCandidates.contains(definition)) {
          definition->emitError(
              "partition input is not available from an earlier plan");
          return failure();
        }
        mapped = input;
      }
      inputs.push_back(mapped);
    }

    llvm::SmallVector<Type> resultTypes;
    for (Value output : originalOutputs)
      resultTypes.push_back(output.getType());

    OpBuilder builder(plan.anchor);
    auto part =
        builder.create<pyc::CombOp>(plan.anchor->getLoc(), resultTypes, inputs);
    llvm::SmallVector<Attribute> promotedNames;
    bool hasPromotedName = false;
    promotedNames.reserve(originalOutputs.size());
    for (Value output : originalOutputs) {
      StringAttr name;
      if (Operation *definition = output.getDefiningOp();
          definition && definition->getNumResults() == 1)
        name = definition->getAttrOfType<StringAttr>("pyc.name");
      hasPromotedName |= name && !name.getValue().empty();
      promotedNames.push_back(name ? Attribute(name)
                                   : Attribute(builder.getStringAttr("")));
    }
    if (hasPromotedName)
      part->setAttr(kCombResultNamesAttr,
                    builder.getArrayAttr(promotedNames));
    stampPartition(part, plan.parentId, partId, plan.ends.size(), end - begin,
                   maxNodes);

    Block *body = new Block();
    part.getBody().push_back(body);
    for (Value input : inputs)
      body->addArgument(input.getType(), plan.anchor->getLoc());

    IRMapping localMapping;
    for (auto [input, argument] :
         llvm::zip(originalInputs, body->getArguments()))
      localMapping.map(input, argument);

    builder.setInsertionPointToStart(body);
    for (unsigned i = begin; i < end; ++i) {
      Operation *cloned = builder.clone(*plan.order[i], localMapping);
      for (auto [oldResult, newResult] :
           llvm::zip(plan.order[i]->getResults(), cloned->getResults()))
        localMapping.map(oldResult, newResult);
    }

    llvm::SmallVector<Value> yielded;
    for (Value output : originalOutputs)
      yielded.push_back(localMapping.lookup(output));
    builder.create<pyc::YieldOp>(plan.anchor->getLoc(), yielded);

    for (auto [output, result] : llvm::zip(originalOutputs, part.getResults()))
      outerMapping.map(output, result);
    begin = end;
  }
  return success();
}

struct PartitionCombPass
    : public PassWrapper<PartitionCombPass, OperationPass<ModuleOp>> {
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
    return "Build one function-level CombDepGraph and directly materialize "
           "GSIM-style sibling pyc.comb runtime units";
  }

  void runOnOperation() override {
    if (maxNodes == 0)
      return;

    ModuleOp module = getOperation();
    llvm::SmallVector<func::FuncOp> functions;
    for (func::FuncOp function : module.getOps<func::FuncOp>()) {
      if (function.isDeclaration() || isStructuralFunction(function))
        continue;
      if (!llvm::hasSingleElement(function.getBody())) {
        function.emitOpError(
            "unified comb partitioning requires a single-block function");
        signalPassFailure();
        return;
      }
      functions.push_back(function);
    }

    // Phase A is read-only.  Freeze the hierarchical semantic contract while
    // every callee still has its original body.  Running this as one module
    // pass is essential: a nested FuncOp pass is allowed to execute in
    // parallel and must not inspect a sibling function that another worker is
    // rewriting.
    {
      CombDepGraphCache preflightCache(module);
      for (func::FuncOp function : functions) {
        auto graph = FunctionCombDepGraph::build(function, preflightCache);
        if (failed(graph) || failed((*graph)->stableTopologicalOrder())) {
          function.emitOpError(
              "cannot partition an invalid or cyclic CombDepGraph");
          signalPassFailure();
          return;
        }
      }
    }

    // Old combs are transparent input syntax.  Unfold every function before
    // constructing any final plan, so an earlier materialization can never
    // perturb a later caller/callee analysis.
    for (func::FuncOp function : functions) {
      if (failed(unfoldExistingCombs(function))) {
        signalPassFailure();
        return;
      }
    }

    // Phase B builds and freezes every function plan against one immutable
    // unfolded module.  No IR is materialized until this loop has completed.
    llvm::SmallVector<FunctionPartitionPlan> frozenPlans;
    CombDepGraphCache graphCache(module);
    for (func::FuncOp function : functions) {
      Block &block = function.getBody().front();
      llvm::SmallVector<Operation *> candidates;
      for (Operation &op : block) {
        if (!isMemoizableCombOperation(&op))
          continue;
        if (failed(verifyMemoizableOperation(&op))) {
          signalPassFailure();
          return;
        }
        candidates.push_back(&op);
      }

      auto graph = FunctionCombDepGraph::build(function, graphCache);
      if (failed(graph)) {
        function.emitOpError("failed to build the unified CombDepGraph");
        signalPassFailure();
        return;
      }
      llvm::SmallVector<unsigned> cycle;
      if (failed((*graph)->stableTopologicalOrder(&cycle))) {
        function.emitOpError("unified CombDepGraph contains a cycle");
        signalPassFailure();
        return;
      }
      CandidateDepProjection projection =
          buildCandidateProjection(candidates, **graph);
      auto order = stableTopologicalOrder(projection);
      if (failed(order)) {
        function.emitOpError(
            "memoizable projection of CombDepGraph is cyclic");
        signalPassFailure();
        return;
      }
      auto stages = planStages(*order, block, maxNodes, projection);
      if (failed(stages)) {
        function.emitOpError("failed to find a legal unified partition plan");
        signalPassFailure();
        return;
      }
      frozenPlans.push_back(FunctionPartitionPlan{
          function, std::move(candidates), std::move(*stages)});
    }

    // Phase C performs the only rewrite.  Each final sibling pyc.comb is
    // emitted directly from its frozen interval; there is no intermediate
    // FuseComb partition boundary and no backend-side semantic recovery.
    for (FunctionPartitionPlan &functionPlan : frozenPlans) {
      func::FuncOp function = functionPlan.function;
      llvm::DenseSet<Operation *> candidateSet(
          functionPlan.candidates.begin(), functionPlan.candidates.end());
      IRMapping outerMapping;
      uint64_t totalParts = 0;
      uint64_t totalWork = 0;
      for (const StagePlan &stage : functionPlan.stages) {
        if (failed(materializeStage(stage, maxNodes, candidateSet,
                                    outerMapping))) {
          signalPassFailure();
          return;
        }
        totalParts += stage.ends.size();
        totalWork += stage.order.size();
      }

      for (Operation *op : functionPlan.candidates) {
        for (Value result : op->getResults()) {
          if (Value replacement = outerMapping.lookupOrNull(result))
            result.replaceAllUsesWith(replacement);
        }
      }
      for (Operation *op : llvm::reverse(functionPlan.candidates))
        op->erase();

      MLIRContext *context = function.getContext();
      Type i64 = IntegerType::get(context, 64);
      function->setAttr(kCombPartitionFunctionPlanAttr,
                        StringAttr::get(context, kCombPartitionPlanVersion));
      function->setAttr(kCombPartitionFunctionPartsAttr,
                        IntegerAttr::get(i64, totalParts));
      function->setAttr(kCombPartitionFunctionWorkAttr,
                        IntegerAttr::get(i64, totalWork));
    }
  }
};

} // namespace

std::unique_ptr<mlir::Pass> createPartitionCombPass(unsigned maxNodes) {
  return std::make_unique<PartitionCombPass>(maxNodes);
}

static PassRegistration<PartitionCombPass> pass;

} // namespace pyc
