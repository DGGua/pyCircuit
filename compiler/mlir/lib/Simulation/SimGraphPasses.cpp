#include "pyc/Simulation/SimGraphPasses.h"

#include "pyc/Dialect/PYC/PYCOps.h"

#include "mlir/Dialect/Arith/IR/Arith.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/SmallVector.h"

#include <algorithm>
#include <limits>
#include <vector>

using namespace mlir;

namespace pyc {
namespace {

static bool isPureGraphNode(const SimNode &node) {
  return node.kind == SimNodeKind::Expression ||
         node.kind == SimNodeKind::PureGroup;
}

// Form initial pure DAG components inside each effect-free run. Independent
// interleaved chains stay separate, so a change in one chain does not activate
// unrelated work. Each component keeps source topological order.
static void runConsecutiveCombGroupingPass(SimGraph &graph) {
  llvm::SmallVector<SimNode, 0> grouped;
  llvm::SmallVector<SimNode, 0> &source = graph.topNodes;
  for (size_t begin = 0; begin < source.size();) {
    if (!isPureGraphNode(source[begin])) {
      grouped.push_back(std::move(source[begin++]));
      continue;
    }
    size_t end = begin + 1;
    while (end < source.size() && isPureGraphNode(source[end]))
      ++end;
    unsigned count = static_cast<unsigned>(end - begin);
    llvm::SmallVector<unsigned> parent;
    llvm::DenseMap<unsigned, unsigned> producer;
    for (unsigned i = 0; i < count; ++i) {
      parent.push_back(i);
      const SimExpr &expr = graph.expressions[source[begin + i].expressionIds[0]];
      producer.try_emplace(expr.result, i);
    }
    auto root = [&](unsigned i) {
      while (parent[i] != i) {
        parent[i] = parent[parent[i]];
        i = parent[i];
      }
      return i;
    };
    for (unsigned i = 0; i < count; ++i) {
      const SimExpr &expr = graph.expressions[source[begin + i].expressionIds[0]];
      for (unsigned operand : expr.operands)
        if (auto it = producer.find(operand); it != producer.end())
          parent[root(i)] = root(it->second);
    }
    llvm::DenseMap<unsigned, unsigned> componentIndex;
    llvm::SmallVector<llvm::SmallVector<unsigned>> components;
    for (unsigned i = 0; i < count; ++i) {
      unsigned id = root(i);
      auto [it, inserted] = componentIndex.try_emplace(id, components.size());
      if (inserted)
        components.emplace_back();
      components[it->second].push_back(i);
    }
    llvm::sort(components, [](const auto &a, const auto &b) {
      return a.front() < b.front();
    });
    for (const auto &members : components) {
      llvm::DenseSet<unsigned> produced;
      llvm::DenseMap<unsigned, unsigned> internalUses;
      for (unsigned i : members) {
        const SimExpr &expr = graph.expressions[source[begin + i].expressionIds[0]];
        produced.insert(expr.result);
      }
      for (unsigned i : members) {
        const SimExpr &expr = graph.expressions[source[begin + i].expressionIds[0]];
        for (unsigned operand : expr.operands)
          if (produced.contains(operand))
            ++internalUses[operand];
      }
      bool hasLiveOut = false;
      for (unsigned i : members) {
        const SimExpr &expr = graph.expressions[source[begin + i].expressionIds[0]];
        const SimValue &value = graph.values[expr.result];
        hasLiveOut |= value.observable ||
                      value.sourceUseCount > internalUses.lookup(expr.result);
      }
      if (members.size() < 2 || !hasLiveOut) {
        for (unsigned i : members)
          grouped.push_back(std::move(source[begin + i]));
        continue;
      }
      SimNode node;
      node.kind = SimNodeKind::PureGroup;
      node.op = source[begin + members.front()].op;
      llvm::DenseSet<unsigned> seenInputs;
      for (unsigned i : members) {
        SimNode &member = source[begin + i];
        node.operations.push_back(member.op);
        node.expressionIds.append(member.expressionIds.begin(),
                                  member.expressionIds.end());
        node.outputs.append(member.outputs.begin(), member.outputs.end());
        node.outputIds.append(member.outputIds.begin(), member.outputIds.end());
        const SimExpr &expr = graph.expressions[member.expressionIds[0]];
        for (unsigned operand : expr.operands)
          if (!produced.contains(operand) && seenInputs.insert(operand).second) {
            node.inputs.push_back(graph.values[operand].source);
            node.inputIds.push_back(operand);
          }
      }
      graph.groupedNodes.push_back(grouped.size());
      grouped.push_back(std::move(node));
    }
    begin = end;
  }
  source = std::move(grouped);
}

// Partition a pure operation DAG without crossing non-combinational nodes.
// Use GSIM's initial-partition recurrence: for each candidate interval, count
// outgoing edges and subtract incoming edges whose producer is already inside
// the interval. Dynamic programming minimizes this cut cost under the bound.
static void runSuperNodePartitionPass(SimGraph &graph, unsigned maxSize) {
  if (maxSize == 0)
    return;
  llvm::SmallVector<SimNode, 0> partitioned;
  llvm::SmallVector<unsigned> groupedIndices;
  for (SimNode &source : graph.topNodes) {
    unsigned size = source.operations.size();
    if (size <= maxSize) {
      if (size > 1)
        groupedIndices.push_back(partitioned.size());
      partitioned.push_back(std::move(source));
      continue;
    }

    llvm::DenseMap<unsigned, unsigned> producer;
    for (auto [i, id] : llvm::enumerate(source.expressionIds))
      producer.try_emplace(graph.expressions[id].result,
                           static_cast<unsigned>(i));
    std::vector<std::vector<unsigned>> predecessors(size), successors(size);
    for (auto [i, id] : llvm::enumerate(source.expressionIds))
      for (unsigned input : graph.expressions[id].operands)
        if (auto it = producer.find(input);
            it != producer.end() && it->second < i) {
          unsigned p = it->second;
          predecessors[i].push_back(p);
          successors[p].push_back(static_cast<unsigned>(i));
        }
    for (unsigned i = 0; i < size; ++i) {
      llvm::sort(predecessors[i]);
      predecessors[i].erase(std::unique(predecessors[i].begin(),
                                        predecessors[i].end()),
                            predecessors[i].end());
      llvm::sort(successors[i]);
      successors[i].erase(std::unique(successors[i].begin(), successors[i].end()),
                          successors[i].end());
    }
    std::vector<int64_t> best(size + 1, std::numeric_limits<int64_t>::max());
    std::vector<unsigned> previous(size + 1, 0);
    best[0] = 0;
    for (unsigned begin = 0; begin < size; ++begin) {
      int64_t intervalCost = 0;
      unsigned limit = begin + std::min(maxSize, size - begin);
      for (unsigned end = begin + 1; end <= limit; ++end) {
        unsigned member = end - 1;
        intervalCost += successors[member].size();
        for (unsigned pred : predecessors[member])
          if (pred >= begin)
            --intervalCost;
        int64_t candidate = best[begin] + intervalCost;
        if (candidate < best[end]) {
          best[end] = candidate;
          previous[end] = begin;
        }
      }
    }
    std::vector<unsigned> cuts;
    for (unsigned end = size; end; end = previous[end])
      cuts.push_back(end);
    cuts.push_back(0);
    std::reverse(cuts.begin(), cuts.end());
    for (size_t part = 1; part < cuts.size(); ++part) {
      unsigned begin = cuts[part - 1], end = cuts[part];
      SimNode node;
      node.kind = SimNodeKind::PureGroup;
      node.op = source.operations[begin];
      llvm::DenseSet<unsigned> partResults;
      for (unsigned i = begin; i < end; ++i)
        partResults.insert(graph.expressions[source.expressionIds[i]].result);
      llvm::DenseSet<unsigned> seenInputs;
      for (unsigned i = begin; i < end; ++i) {
        Operation *op = source.operations[i];
        node.operations.push_back(op);
        node.expressionIds.push_back(source.expressionIds[i]);
        unsigned result = graph.expressions[source.expressionIds[i]].result;
        node.outputs.push_back(graph.values[result].source);
        node.outputIds.push_back(result);
        for (unsigned input : graph.expressions[source.expressionIds[i]].operands) {
          if (!partResults.contains(input) && seenInputs.insert(input).second) {
            node.inputs.push_back(graph.values[input].source);
            node.inputIds.push_back(input);
          }
        }
      }
      if (end - begin > 1)
        groupedIndices.push_back(partitioned.size());
      partitioned.push_back(std::move(node));
    }
  }
  graph.topNodes = std::move(partitioned);
  graph.groupedNodes = std::move(groupedIndices);
}

static void rebuildGroupInputs(SimGraph &graph, SimNode &group) {
  llvm::DenseSet<unsigned> produced;
  for (unsigned id : group.replicatedExpressionIds)
    produced.insert(graph.expressions[id].result);
  for (unsigned id : group.expressionIds)
    produced.insert(graph.expressions[id].result);
  group.inputs.clear();
  group.inputIds.clear();
  llvm::DenseSet<unsigned> seen;
  auto collect = [&](unsigned id) {
    for (unsigned operand : graph.expressions[id].operands)
      if (!produced.contains(operand) && seen.insert(operand).second) {
        group.inputIds.push_back(operand);
        group.inputs.push_back(graph.values[operand].source);
      }
  };
  for (unsigned id : group.replicatedExpressionIds)
    collect(id);
  for (unsigned id : group.expressionIds)
    collect(id);
}

// GSIM merges independent condition-controlled nodes before partitioning.
// PYC represents these choices as mux values. Gather source-backed scalar
// muxes with one selector inside an effect-free run when every operand is
// available before the first mux. This permits the later statement planner to
// emit one branch even when unrelated DAG nodes separate the muxes.
static void runSharedMuxGroupingPass(SimGraph &graph, unsigned maxSize) {
  if (maxSize < 6)
    return;
  llvm::DenseMap<unsigned, llvm::SmallVector<unsigned>> at;
  llvm::DenseSet<unsigned> selected;
  for (unsigned begin = 0; begin < graph.topNodes.size();) {
    if (!isPureGraphNode(graph.topNodes[begin])) {
      ++begin;
      continue;
    }
    unsigned end = begin + 1;
    while (end < graph.topNodes.size() && isPureGraphNode(graph.topNodes[end]))
      ++end;
    llvm::DenseMap<unsigned, unsigned> producer;
    for (unsigned i = begin; i < end; ++i)
      for (unsigned value : graph.topNodes[i].outputIds)
        producer.try_emplace(value, i);
    auto eligible = [&](unsigned index) {
      const SimNode &node = graph.topNodes[index];
      if (node.kind != SimNodeKind::Expression ||
          node.expressionIds.size() != 1 || node.operations.size() != 1)
        return false;
      const SimExpr &expr = graph.expressions[node.expressionIds.front()];
      if (expr.kind != SimExprKind::Mux || expr.operands.size() != 3 ||
          !graph.values[expr.result].type.shape.empty() ||
          !graph.values[expr.result].sourceBacked)
        return false;
      const SimType &selector = graph.values[expr.operands[0]].type;
      return selector.shape.empty() && selector.width == 1 &&
             llvm::all_of(expr.operands, [&](unsigned operand) {
               return graph.values[operand].sourceBacked;
             });
    };
    for (unsigned first = begin; first < end; ++first) {
      if (selected.contains(first) || !eligible(first))
        continue;
      unsigned selector =
          graph.expressions[graph.topNodes[first].expressionIds.front()]
              .operands[0];
      llvm::SmallVector<unsigned> matches;
      for (unsigned i = first; i < end && matches.size() < maxSize; ++i) {
        if (selected.contains(i) || !eligible(i))
          continue;
        const SimExpr &expr =
            graph.expressions[graph.topNodes[i].expressionIds.front()];
        if (expr.operands[0] != selector ||
            !llvm::all_of(expr.operands, [&](unsigned operand) {
              auto it = producer.find(operand);
              return it == producer.end() || it->second < first;
            }))
          continue;
        matches.push_back(i);
      }
      if (matches.size() < 6)
        continue;
      at.try_emplace(first, matches);
      for (unsigned i : matches)
        selected.insert(i);
    }
    begin = end;
  }
  if (at.empty())
    return;
  llvm::DenseSet<unsigned> oldGroups(graph.groupedNodes.begin(),
                                     graph.groupedNodes.end());
  llvm::SmallVector<SimNode, 0> grouped;
  llvm::SmallVector<unsigned> groupIds;
  for (unsigned i = 0; i < graph.topNodes.size(); ++i) {
    if (auto found = at.find(i); found != at.end()) {
      SimNode node;
      node.kind = SimNodeKind::PureGroup;
      for (unsigned memberId : found->second) {
        const SimNode &member = graph.topNodes[memberId];
        node.operations.append(member.operations.begin(),
                               member.operations.end());
        node.expressionIds.append(member.expressionIds.begin(),
                                  member.expressionIds.end());
        node.outputs.append(member.outputs.begin(), member.outputs.end());
        node.outputIds.append(member.outputIds.begin(), member.outputIds.end());
      }
      node.op = node.operations.front();
      rebuildGroupInputs(graph, node);
      groupIds.push_back(grouped.size());
      grouped.push_back(std::move(node));
    } else if (!selected.contains(i)) {
      if (oldGroups.contains(i))
        groupIds.push_back(grouped.size());
      grouped.push_back(std::move(graph.topNodes[i]));
    }
  }
  graph.topNodes = std::move(grouped);
  graph.groupedNodes = std::move(groupIds);
}

// GSIM's initial partition is global over the ordered supernode DAG, not only
// a split of an oversized supernode. Keep effectful PYC nodes as hard cuts:
// they may write wires or state that a pure expression reads. Each pure run
// uses the same bounded interval recurrence as graphInitPartition.
static void runGlobalInitialPartitionPass(SimGraph &graph, unsigned maxSize) {
  if (maxSize < 2)
    return;
  const unsigned count = graph.topNodes.size();
  std::vector<llvm::DenseSet<unsigned>> allPrev(count), allNext(count);
  for (const SimEdge &edge : graph.edges) {
    allNext[edge.producer].insert(edge.consumer);
    allPrev[edge.consumer].insert(edge.producer);
  }
  llvm::SmallVector<SimNode, 0> result;
  llvm::SmallVector<unsigned> grouped;
  auto append = [&](SimNode &&node) {
    if (node.operations.size() > 1)
      grouped.push_back(result.size());
    result.push_back(std::move(node));
  };
  for (unsigned runBegin = 0; runBegin < count;) {
    auto eligible = [&](unsigned id) {
      const SimNode &node = graph.topNodes[id];
      return !node.stateBoundary && isPureGraphNode(node) &&
             !node.operations.empty() && node.operations.size() <= maxSize;
    };
    if (!eligible(runBegin)) {
      append(std::move(graph.topNodes[runBegin++]));
      continue;
    }
    unsigned runEnd = runBegin + 1;
    while (runEnd < count && eligible(runEnd))
      ++runEnd;
    const unsigned length = runEnd - runBegin;
    if (length == 1) {
      append(std::move(graph.topNodes[runBegin]));
      runBegin = runEnd;
      continue;
    }
    bool ordered = true;
    for (unsigned i = runBegin; i < runEnd; ++i)
      for (unsigned predecessor : allPrev[i])
        if (predecessor >= runBegin && predecessor < runEnd &&
            predecessor >= i)
          ordered = false;
    if (!ordered) {
      for (unsigned i = runBegin; i < runEnd; ++i)
        append(std::move(graph.topNodes[i]));
      runBegin = runEnd;
      continue;
    }
    std::vector<int64_t> best(length + 1, std::numeric_limits<int64_t>::max());
    std::vector<unsigned> previous(length + 1, 0);
    best[0] = 0;
    for (unsigned begin = 0; begin < length; ++begin) {
      unsigned accumulated = 0;
      int64_t intervalCost = 0;
      for (unsigned end = begin; end < length; ++end) {
        accumulated += graph.topNodes[runBegin + end].operations.size();
        if (accumulated > maxSize)
          break;
        intervalCost += allNext[runBegin + end].size();
        for (unsigned predecessor : allPrev[runBegin + end])
          if (predecessor >= runBegin + begin &&
              predecessor < runBegin + end)
            --intervalCost;
        int64_t candidate = best[begin] + intervalCost;
        if (candidate < best[end + 1]) {
          best[end + 1] = candidate;
          previous[end + 1] = begin;
        }
      }
    }
    std::vector<unsigned> cuts;
    for (unsigned end = length; end; end = previous[end])
      cuts.push_back(end);
    cuts.push_back(0);
    std::reverse(cuts.begin(), cuts.end());
    for (unsigned part = 1; part < cuts.size(); ++part) {
      unsigned begin = runBegin + cuts[part - 1];
      unsigned end = runBegin + cuts[part];
      if (end == begin + 1) {
        append(std::move(graph.topNodes[begin]));
        continue;
      }
      llvm::SmallVector<std::pair<unsigned, unsigned>> expressions;
      for (unsigned i = begin; i < end; ++i)
        for (unsigned id : graph.topNodes[i].expressionIds)
          expressions.emplace_back(graph.expressions[id].sourceOrder, id);
      llvm::sort(expressions);
      SimNode node;
      node.kind = SimNodeKind::PureGroup;
      for (auto [order, id] : expressions) {
        (void)order;
        const SimExpr &expr = graph.expressions[id];
        node.operations.push_back(expr.source);
        node.expressionIds.push_back(id);
        node.outputs.push_back(graph.values[expr.result].source);
        node.outputIds.push_back(expr.result);
      }
      node.op = node.operations.front();
      rebuildGroupInputs(graph, node);
      append(std::move(node));
    }
    runBegin = runEnd;
  }
  graph.topNodes = std::move(result);
  graph.groupedNodes = std::move(grouped);
}

// Coarsen the execution DAG before the bounded initial partition. This is
// GSIM's out-degree-one, in-degree-one, and sibling grouping on graph nodes.
// Effectful nodes participate in the dependency test but are never merged.
// A quotient cycle would change evaluation order, so reject that merge.
static void runGraphCoarseningPass(SimGraph &graph) {
  const unsigned count = graph.topNodes.size();
  if (count < 2)
    return;
  std::vector<unsigned> parent(count), opCount(count);
  for (unsigned i = 0; i < count; ++i) {
    parent[i] = i;
    opCount[i] = graph.topNodes[i].operations.size();
  }
  auto root = [&](unsigned i) {
    while (parent[i] != i) {
      parent[i] = parent[parent[i]];
      i = parent[i];
    }
    return i;
  };
  auto pure = [&](unsigned i) {
    const SimNode &node = graph.topNodes[i];
    return !node.stateBoundary && isPureGraphNode(node);
  };
  bool changedAny = false;
  enum class MergeKind { OutOne, InOne, Sibling };
  for (MergeKind kind : {MergeKind::OutOne, MergeKind::InOne,
                         MergeKind::Sibling}) {
    for (;;) {
      std::vector<llvm::DenseSet<unsigned>> prev(count), next(count);
      for (const SimEdge &edge : graph.edges) {
        unsigned from = root(edge.producer), to = root(edge.consumer);
        if (from != to) {
          next[from].insert(to);
          prev[to].insert(from);
        }
      }
      auto hasAlternatePath = [&](unsigned from, unsigned to) {
        llvm::DenseSet<unsigned> seen;
        llvm::SmallVector<unsigned> todo;
        seen.insert(from);
        for (unsigned successor : next[from])
          if (successor != to && seen.insert(successor).second)
            todo.push_back(successor);
        while (!todo.empty()) {
          unsigned current = todo.pop_back_val();
          if (current == to)
            return true;
          for (unsigned successor : next[current])
            if (seen.insert(successor).second)
              todo.push_back(successor);
        }
        return false;
      };
      llvm::DenseSet<unsigned> used;
      unsigned merged = 0;
      auto tryMerge = [&](unsigned a, unsigned b, unsigned sizeLimit) {
        if (a == b || used.contains(a) || used.contains(b) ||
            !pure(a) || !pure(b) ||
            opCount[a] + opCount[b] > sizeLimit ||
            (next[a].contains(b) && next[b].contains(a)) ||
            hasAlternatePath(a, b) || hasAlternatePath(b, a))
          return false;
        // Preserve the earlier source node as the quotient representative.
        if (a > b)
          std::swap(a, b);
        parent[b] = a;
        opCount[a] += opCount[b];
        used.insert(a);
        used.insert(b);
        ++merged;
        return true;
      };
      if (kind == MergeKind::OutOne) {
        for (unsigned i = count; i-- > 0;)
          if (root(i) == i && pure(i) && next[i].size() == 1)
            tryMerge(i, *next[i].begin(), 7000);
      } else if (kind == MergeKind::InOne) {
        for (unsigned i = 0; i < count; ++i)
          if (root(i) == i && pure(i) && prev[i].size() == 1)
            tryMerge(i, *prev[i].begin(), 7000);
      } else {
        llvm::DenseMap<uint64_t, unsigned> pendingSibling;
        for (unsigned i = 0; i < count; ++i) {
          if (root(i) != i || !pure(i) || prev[i].empty() ||
              opCount[i] >= 30)
            continue;
          uint64_t signature = prev[i].size();
          for (unsigned predecessor : prev[i])
            signature += (static_cast<uint64_t>(predecessor) + 1) *
                         UINT64_C(0x9e3779b97f4a7c15);
          if (auto it = pendingSibling.find(signature);
              it != pendingSibling.end()) {
            unsigned other = it->second;
            bool same = prev[other].size() == prev[i].size() &&
                        llvm::all_of(prev[i], [&](unsigned p) {
                          return prev[other].contains(p);
                        });
            if (same && tryMerge(other, i, 58)) {
              pendingSibling.erase(it);
              continue;
            }
          }
          pendingSibling[signature] = i;
        }
      }
      if (!merged)
        break;
      changedAny = true;
    }
  }
  if (!changedAny)
    return;

  std::vector<std::vector<unsigned>> members(count);
  for (unsigned i = 0; i < count; ++i)
    members[root(i)].push_back(i);
  llvm::SmallVector<SimNode, 0> coarsened;
  llvm::SmallVector<unsigned> groupedIndices;
  for (unsigned i = 0; i < count; ++i) {
    if (members[i].empty())
      continue;
    if (members[i].size() == 1) {
      SimNode &node = graph.topNodes[i];
      if (node.operations.size() > 1)
        groupedIndices.push_back(coarsened.size());
      coarsened.push_back(std::move(node));
      continue;
    }
    llvm::SmallVector<std::pair<unsigned, unsigned>> expressions;
    for (unsigned member : members[i]) {
      const SimNode &node = graph.topNodes[member];
      for (unsigned id : node.expressionIds)
        expressions.emplace_back(graph.expressions[id].sourceOrder, id);
    }
    llvm::sort(expressions);
    SimNode node;
    node.kind = SimNodeKind::PureGroup;
    for (auto [order, id] : expressions) {
      (void)order;
      const SimExpr &expr = graph.expressions[id];
      node.operations.push_back(expr.source);
      node.expressionIds.push_back(id);
      node.outputs.push_back(graph.values[expr.result].source);
      node.outputIds.push_back(expr.result);
    }
    node.op = node.operations.front();
    rebuildGroupInputs(graph, node);
    groupedIndices.push_back(coarsened.size());
    coarsened.push_back(std::move(node));
  }
  graph.topNodes = std::move(coarsened);
  graph.groupedNodes = std::move(groupedIndices);
}

// GSIM removes a singleton node when the expression cost times the number of
// consumer supernodes is below its singleton threshold of three. The copies
// are graph expressions and never rewrite legalized hardware IR.
static int replicationOpCost(SimExprKind kind) {
  switch (kind) {
  case SimExprKind::Constant:
  case SimExprKind::Alias:
    return 0;
  case SimExprKind::ResetActive:
  case SimExprKind::Add:
  case SimExprKind::Sub:
  case SimExprKind::Mul:
  case SimExprKind::Udiv:
  case SimExprKind::Urem:
  case SimExprKind::Sdiv:
  case SimExprKind::Srem:
  case SimExprKind::Mux:
  case SimExprKind::Select:
  case SimExprKind::And:
  case SimExprKind::Or:
  case SimExprKind::Xor:
  case SimExprKind::Not:
  case SimExprKind::Concat:
  case SimExprKind::Eq:
  case SimExprKind::Ult:
  case SimExprKind::Slt:
  case SimExprKind::Trunc:
  case SimExprKind::Zext:
  case SimExprKind::Sext:
  case SimExprKind::Extract:
  case SimExprKind::Shli:
  case SimExprKind::Lshri:
  case SimExprKind::Ashri:
  case SimExprKind::Shl:
  case SimExprKind::Lshr:
  case SimExprKind::Ashr:
  case SimExprKind::VGet:
    return 1;
  default:
    return -1;
  }
}

static void runReplicationPass(SimGraph &graph) {
  llvm::DenseMap<unsigned, unsigned> producerByValue;
  for (auto [id, expr] : llvm::enumerate(graph.expressions))
    producerByValue.try_emplace(expr.result, static_cast<unsigned>(id));
  llvm::DenseSet<unsigned> requiredMaterialized;
  llvm::DenseMap<unsigned, unsigned> groupByExpression;
  llvm::DenseSet<unsigned> grouped(graph.groupedNodes.begin(),
                                   graph.groupedNodes.end());
  for (auto [index, node] : llvm::enumerate(graph.topNodes))
    if (node.kind == SimNodeKind::Expression ||
        node.kind == SimNodeKind::PureGroup)
      for (unsigned id : node.expressionIds)
        groupByExpression.try_emplace(id, static_cast<unsigned>(index));
  for (const SimNode &source : graph.topNodes) {
    if (source.operations.size() != 1 || source.expressionIds.size() != 1)
      continue;
    unsigned sourceId = source.expressionIds[0];
    const SimExpr original = graph.expressions[sourceId];
    const SimValue value = graph.values[original.result];
    if (value.observable || requiredMaterialized.contains(original.result) ||
        !value.type.shape.empty() ||
        value.type.width > 64 || value.sourceUseCount == 0)
      continue;
    int cost = replicationOpCost(original.kind);
    if (cost < 0)
      continue;
    llvm::DenseSet<unsigned> localSynthetic;
    for (unsigned id : source.replicatedExpressionIds)
      localSynthetic.insert(graph.expressions[id].result);
    std::function<int(unsigned)> operandCost = [&](unsigned id) -> int {
      const SimValue &operand = graph.values[id];
      auto producer = producerByValue.find(id);
      // GSIM permits a cheap scalar read from an already materialized array.
      // The vector itself must remain available; copying it would duplicate
      // aggregate construction and invalidate the singleton cost model.
      if (!operand.type.shape.empty()) {
        if (!operand.sourceBacked ||
            (producer != producerByValue.end() &&
             graph.expressions[producer->second].omitOriginalEvaluation))
          return -1;
        return 0;
      }
      if (operand.type.width > 64)
        return -1;
      if (operand.sourceBacked)
        return producer != producerByValue.end() &&
                       graph.expressions[producer->second].omitOriginalEvaluation
                   ? -1 : 0;
      if (!localSynthetic.contains(id) || producer == producerByValue.end())
        return -1;
      const SimExpr &inner = graph.expressions[producer->second];
      if (graph.values[inner.result].sourceBacked ||
          !inner.inlineIntoConsumer)
        return -1;
      int result = replicationOpCost(inner.kind);
      if (result < 0)
        return -1;
      for (unsigned input : inner.operands) {
        int child = operandCost(input);
        if (child < 0)
          return -1;
        result += child;
        if (result >= 3)
          return result;
      }
      return result;
    };
    for (unsigned operand : original.operands) {
      int operandOperationCount = operandCost(operand);
      if (operandOperationCount < 0) {
        cost = -1;
        break;
      }
      cost += operandOperationCount;
    }
    if (cost < 0)
      continue;
    llvm::SmallVector<std::pair<unsigned, unsigned>> consumers;
    llvm::DenseSet<unsigned> distinctUsers;
    for (unsigned userId : value.expressionUsers) {
      auto it = groupByExpression.find(userId);
      if (it == groupByExpression.end()) {
        consumers.clear();
        break;
      }
      consumers.emplace_back(it->second, userId);
      distinctUsers.insert(userId);
    }
    if (consumers.size() != value.sourceUseCount)
      continue;
    if (static_cast<unsigned>(cost) * distinctUsers.size() >= 3u)
      continue;
    llvm::SmallVector<unsigned> orderedGroups;
    for (auto [groupIndex, consumerId] : consumers) {
      (void)consumerId;
      if (llvm::find(orderedGroups, groupIndex) == orderedGroups.end())
        orderedGroups.push_back(groupIndex);
    }
    for (unsigned groupIndex : orderedGroups) {
      llvm::DenseMap<unsigned, unsigned> copiedValues;
      std::function<unsigned(unsigned)> copyOperand = [&](unsigned id) -> unsigned {
        if (graph.values[id].sourceBacked)
          return id;
        if (auto it = copiedValues.find(id); it != copiedValues.end())
          return it->second;
        SimExpr inner = graph.expressions[producerByValue.lookup(id)];
        for (unsigned &operand : inner.operands)
          operand = copyOperand(operand);
        unsigned newValue = graph.values.size();
        graph.values.push_back(SimValue{graph.values[id].type, Value{}, 1, false});
        inner.source = nullptr;
        inner.result = newValue;
        inner.inlineIntoConsumer = true;
        inner.omitOriginalEvaluation = false;
        unsigned newExpression = graph.expressions.size();
        graph.expressions.push_back(std::move(inner));
        producerByValue.try_emplace(newValue, newExpression);
        graph.topNodes[groupIndex].replicatedExpressionIds.push_back(newExpression);
        copiedValues.try_emplace(id, newValue);
        return newValue;
      };
      unsigned useCount = 0;
      llvm::DenseSet<unsigned> groupUsers;
      for (auto [consumerGroup, consumerId] : consumers)
        if (consumerGroup == groupIndex && groupUsers.insert(consumerId).second)
          for (unsigned operand : graph.expressions[consumerId].operands)
            useCount += operand == original.result;
      unsigned cloneValue = graph.values.size();
      graph.values.push_back(SimValue{value.type, Value{}, useCount, false});
      SimExpr clone = original;
      for (unsigned &operand : clone.operands)
        operand = copyOperand(operand);
      clone.source = nullptr;
      clone.result = cloneValue;
      clone.inlineIntoConsumer = true;
      clone.omitOriginalEvaluation = false;
      unsigned cloneId = graph.expressions.size();
      graph.expressions.push_back(std::move(clone));
      producerByValue.try_emplace(cloneValue, cloneId);
      graph.topNodes[groupIndex].replicatedExpressionIds.push_back(cloneId);
      if (grouped.insert(groupIndex).second)
        graph.groupedNodes.push_back(groupIndex);
      for (unsigned consumerId : groupUsers)
        for (unsigned &operand : graph.expressions[consumerId].operands)
          if (operand == original.result)
            operand = cloneValue;
      rebuildGroupInputs(graph, graph.topNodes[groupIndex]);
    }
    std::function<void(unsigned)> requireInputs = [&](unsigned id) {
      if (graph.values[id].sourceBacked) {
        if (producerByValue.count(id))
          requiredMaterialized.insert(id);
        return;
      }
      const SimExpr &inner = graph.expressions[producerByValue.lookup(id)];
      for (unsigned operand : inner.operands)
        requireInputs(operand);
    };
    for (unsigned operand : original.operands)
      requireInputs(operand);
    graph.expressions[sourceId].omitOriginalEvaluation = true;
  }
  llvm::SmallVector<SimNode, 0> kept;
  llvm::SmallVector<unsigned> remap(graph.topNodes.size(), ~0u);
  for (auto [index, node] : llvm::enumerate(graph.topNodes)) {
    if (node.operations.size() == 1 && node.expressionIds.size() == 1 &&
        graph.expressions[node.expressionIds.front()].omitOriginalEvaluation)
      continue;
    remap[index] = kept.size();
    kept.push_back(std::move(node));
  }
  llvm::SmallVector<unsigned> groups;
  for (unsigned index : graph.groupedNodes)
    if (remap[index] != ~0u)
      groups.push_back(remap[index]);
  llvm::sort(groups);
  graph.topNodes = std::move(kept);
  graph.groupedNodes = std::move(groups);
}

// Every remaining pure singleton is a one-node supernode. Apply the same
// activity protocol used by multi-expression groups, including packed
// propagation to downstream groups. Constant and alias nodes are cheaper to
// evaluate than to compare and are left as plain assignments.
static void runSingletonActivationPass(SimGraph &graph) {
  llvm::DenseSet<unsigned> existing(graph.groupedNodes.begin(),
                                    graph.groupedNodes.end());
  for (auto [index, node] : llvm::enumerate(graph.topNodes)) {
    if (existing.contains(index) ||
        (node.kind != SimNodeKind::Expression &&
         node.kind != SimNodeKind::PureGroup) ||
        node.expressionIds.size() != 1 || node.operations.size() != 1)
      continue;
    const SimExpr &expr = graph.expressions[node.expressionIds.front()];
    if (expr.omitOriginalEvaluation || expr.kind == SimExprKind::Constant ||
        expr.kind == SimExprKind::Alias)
      continue;
    graph.groupedNodes.push_back(static_cast<unsigned>(index));
  }
  llvm::sort(graph.groupedNodes);
}

// GSIM coalesces a sufficiently large set of FIRRTL when nodes with a common
// condition. PYC represents the corresponding value choice as mux expressions.
// The global partition has already formed execution groups. Move independent
// scalar muxes with a shared selector together, then mark the resulting runs
// for one conditional statement. Every moved mux must read only values that
// were available before the first mux in its run.
static void runMuxConditionBatchPass(SimGraph &graph) {
  llvm::DenseSet<unsigned> inlinedValues;
  for (const SimExpr &expr : graph.expressions)
    if (expr.inlineIntoConsumer)
      inlinedValues.insert(expr.result);
  for (unsigned groupIndex : graph.groupedNodes) {
    SimNode &group = graph.topNodes[groupIndex];
    group.muxConditionBatches.clear();
    auto eligible = [&](unsigned index) {
      const SimExpr &expr = graph.expressions[group.expressionIds[index]];
      if (expr.kind != SimExprKind::Mux || expr.operands.size() != 3 ||
          expr.inlineIntoConsumer || expr.omitOriginalEvaluation ||
          !graph.values[expr.result].type.shape.empty())
        return false;
      const SimType &selector = graph.values[expr.operands[0]].type;
      if (!selector.shape.empty() || selector.width != 1)
        return false;
      for (unsigned operand : expr.operands)
        if (!graph.values[operand].sourceBacked ||
            inlinedValues.contains(operand))
          return false;
      return graph.values[expr.result].sourceBacked;
    };
    llvm::DenseSet<unsigned> produced;
    for (unsigned id : group.expressionIds)
      produced.insert(graph.expressions[id].result);
    for (unsigned begin = 0; begin < group.expressionIds.size(); ++begin) {
      if (!eligible(begin))
        continue;
      unsigned selector = graph.expressions[group.expressionIds[begin]].operands[0];
      llvm::DenseSet<unsigned> availableBefore;
      for (unsigned i = 0; i < begin; ++i)
        availableBefore.insert(graph.expressions[group.expressionIds[i]].result);
      llvm::SmallVector<unsigned> selected;
      for (unsigned i = begin; i < group.expressionIds.size(); ++i) {
        if (!eligible(i))
          continue;
        const SimExpr &expr = graph.expressions[group.expressionIds[i]];
        if (expr.operands[0] != selector ||
            !llvm::all_of(expr.operands, [&](unsigned operand) {
              return !produced.contains(operand) || availableBefore.contains(operand);
            }))
          continue;
        selected.push_back(i);
      }
      if (selected.size() < 6)
        continue;
      bool contiguous = selected.back() - selected.front() + 1 == selected.size();
      if (!contiguous) {
        llvm::DenseSet<unsigned> chosen(selected.begin(), selected.end());
        llvm::SmallVector<unsigned> order;
        for (unsigned i = 0; i < begin; ++i)
          order.push_back(i);
        order.append(selected.begin(), selected.end());
        for (unsigned i = begin; i < group.expressionIds.size(); ++i)
          if (!chosen.contains(i))
            order.push_back(i);
        auto applyOrder = [&](auto &items) {
          auto original = items;
          for (auto [i, oldIndex] : llvm::enumerate(order))
            items[i] = original[oldIndex];
        };
        applyOrder(group.operations);
        applyOrder(group.expressionIds);
        applyOrder(group.outputs);
        applyOrder(group.outputIds);
        group.op = group.operations.front();
      }
      begin += selected.size() - 1;
    }
    for (unsigned begin = 0; begin < group.expressionIds.size();) {
      if (!eligible(begin)) {
        ++begin;
        continue;
      }
      unsigned selector =
          graph.expressions[group.expressionIds[begin]].operands[0];
      unsigned end = begin + 1;
      while (end < group.expressionIds.size() && eligible(end) &&
             graph.expressions[group.expressionIds[end]].operands[0] ==
                 selector)
        ++end;
      if (end - begin > 5)
        group.muxConditionBatches.push_back({begin, end, selector});
      begin = end;
    }
  }
}

// A grouped pure expression has no hidden state. Its external input values
// fully determine its outputs, so the planner may skip it when they are stable.
static void runGroupActivationPass(SimGraph &graph) {
  for (unsigned index : graph.groupedNodes)
    graph.topNodes[index].activateOnInputChange = true;
}

// Inline a cheap expression when all uses stay in one supernode and repeated
// evaluation costs no more than one evaluation plus a materialized node.
// Named/debug values remain materialized for observation.
static void runExpressionInliningPass(SimGraph &graph) {
  for (unsigned index : graph.groupedNodes) {
    const SimNode &group = graph.topNodes[index];
    llvm::DenseMap<unsigned, unsigned> internalUses;
    llvm::DenseMap<unsigned, unsigned> inlinedCost;
    for (unsigned id : group.expressionIds)
      for (unsigned operand : graph.expressions[id].operands)
        ++internalUses[operand];
    for (unsigned id : group.expressionIds) {
      SimExpr &expr = graph.expressions[id];
      const SimValue &value = graph.values[expr.result];
      unsigned uses = internalUses.lookup(expr.result);
      if (!value.type.shape.empty() || value.type.width > 64 ||
          value.observable || !uses || value.sourceUseCount != uses)
        continue;
      unsigned cost = 1;
      switch (expr.kind) {
      case SimExprKind::Constant:
      case SimExprKind::Alias:
        cost = 0;
        break;
      case SimExprKind::ResetActive:
      case SimExprKind::Add:
      case SimExprKind::Sub:
      case SimExprKind::Mul:
      case SimExprKind::And:
      case SimExprKind::Or:
      case SimExprKind::Xor:
      case SimExprKind::Not:
      case SimExprKind::Concat:
      case SimExprKind::Eq:
      case SimExprKind::Ult:
      case SimExprKind::Slt:
      case SimExprKind::Trunc:
      case SimExprKind::Zext:
      case SimExprKind::Sext:
      case SimExprKind::Extract:
      case SimExprKind::Shli:
      case SimExprKind::Lshri:
      case SimExprKind::Ashri:
      case SimExprKind::VGet:
        break;
      case SimExprKind::Shl:
      case SimExprKind::Lshr:
      case SimExprKind::Ashr:
        cost = 2;
        break;
      default:
        continue;
      }
      for (unsigned operand : expr.operands)
        cost = std::min(3u, cost + inlinedCost.lookup(operand));
      // Bound the emitted C++ expression tree even for one-use chains.
      if (cost > 2 || cost * uses > cost + 1)
        continue;
      expr.inlineIntoConsumer = true;
      inlinedCost.try_emplace(expr.result, cost);
    }
  }
}

static unsigned scalarWidth(const SimGraph &graph, unsigned value) {
  const SimType &type = graph.values[value].type;
  return type.shape.empty() ? type.width : 0;
}

template <typename Demand, typename DemandFull>
static void propagateUsedBits(const SimGraph &graph, const SimExpr &expr,
                              const llvm::APInt &bits,
                              const llvm::DenseMap<unsigned, llvm::APInt> &constants,
                              Demand demand,
                              DemandFull demandFull) {
  if (expr.kind == SimExprKind::Extract) {
    unsigned width = scalarWidth(graph, expr.operands[0]);
    if (width && expr.immediate >= 0 &&
        static_cast<uint64_t>(expr.immediate) < width)
      demand(expr.operands[0], bits.zextOrTrunc(width)
                                   .shl(static_cast<unsigned>(expr.immediate)));
  } else if (expr.kind == SimExprKind::And ||
             expr.kind == SimExprKind::Or) {
    for (unsigned i = 0; i < expr.operands.size(); ++i) {
      llvm::APInt needed = bits;
      if (expr.operands.size() == 2) {
        auto other = constants.find(expr.operands[1 - i]);
        if (other != constants.end()) {
          llvm::APInt constant =
              other->second.zextOrTrunc(bits.getBitWidth());
          needed &= expr.kind == SimExprKind::And ? constant : ~constant;
        }
      }
      demand(expr.operands[i], needed);
    }
  } else if (expr.kind == SimExprKind::Xor ||
             expr.kind == SimExprKind::Not ||
             expr.kind == SimExprKind::Alias) {
    for (unsigned input : expr.operands)
      demand(input, bits);
  } else if (expr.kind == SimExprKind::Add ||
             expr.kind == SimExprKind::Sub ||
             expr.kind == SimExprKind::Mul) {
    llvm::APInt prefix = llvm::APInt::getLowBitsSet(
        bits.getBitWidth(), bits.getActiveBits());
    for (unsigned input : expr.operands)
      demand(input, prefix);
  } else if (expr.kind == SimExprKind::Trunc ||
             expr.kind == SimExprKind::Zext) {
    demand(expr.operands[0], bits);
  } else if (expr.kind == SimExprKind::Sext) {
    unsigned input = expr.operands[0];
    unsigned width = scalarWidth(graph, input);
    if (!width) {
      demandFull(input);
      return;
    }
    llvm::APInt inputBits = bits.zextOrTrunc(width);
    if (width < bits.getBitWidth() && !bits.lshr(width).isZero())
      inputBits.setBit(width - 1);
    demand(input, inputBits);
  } else if (expr.kind == SimExprKind::Concat) {
    unsigned lowerWidth = 0;
    for (unsigned input : llvm::reverse(expr.operands)) {
      unsigned width = scalarWidth(graph, input);
      if (width && lowerWidth < bits.getBitWidth())
        demand(input, bits.lshr(lowerWidth).zextOrTrunc(width));
      lowerWidth += width;
    }
  } else if (expr.kind == SimExprKind::Shli) {
    uint64_t amount = expr.immediate;
    if (amount < bits.getBitWidth())
      demand(expr.operands[0], bits.lshr(static_cast<unsigned>(amount)));
  } else if (expr.kind == SimExprKind::Lshri) {
    unsigned width = scalarWidth(graph, expr.operands[0]);
    if (width && expr.immediate >= 0 &&
        static_cast<uint64_t>(expr.immediate) < width)
      demand(expr.operands[0], bits.zextOrTrunc(width)
                                   .shl(static_cast<unsigned>(expr.immediate)));
  } else if (expr.kind == SimExprKind::Ashri) {
    unsigned width = scalarWidth(graph, expr.operands[0]);
    if (width) {
      unsigned amount = static_cast<unsigned>(std::min<uint64_t>(
          expr.immediate, width));
      llvm::APInt inputBits = bits.zextOrTrunc(width).shl(amount);
      if (amount && !bits.lshr(width - amount).isZero())
        inputBits.setBit(width - 1);
      demand(expr.operands[0], inputBits);
    }
  } else if (expr.kind == SimExprKind::Shl ||
             expr.kind == SimExprKind::Lshr ||
             expr.kind == SimExprKind::Ashr) {
    auto amount = constants.find(expr.operands[1]);
    unsigned width = scalarWidth(graph, expr.operands[0]);
    if (!width) {
      for (unsigned input : expr.operands)
        demandFull(input);
      return;
    }
    if (amount == constants.end()) {
      demandFull(expr.operands[1]);
      unsigned amountWidth = scalarWidth(graph, expr.operands[1]);
      if (!amountWidth) {
        demandFull(expr.operands[0]);
        return;
      }
      // The union over every possible shift is an interval dilation of the
      // demanded bits. Doubling the covered interval takes O(log width)
      // APInt shifts, even when the shift amount is wide.
      unsigned maxShift = amountWidth >= 32
                              ? width
                              : std::min(width, (1u << amountWidth) - 1u);
      bool leftShift = expr.kind != SimExprKind::Shl;
      llvm::APInt inputBits = leftShift ? bits.zextOrTrunc(width) : bits;
      unsigned limit = std::min(maxShift, inputBits.getBitWidth() - 1);
      for (unsigned covered = 0; covered < limit;) {
        unsigned step = std::min(covered + 1, limit - covered);
        inputBits |= leftShift ? inputBits.shl(step)
                               : inputBits.lshr(step);
        covered += step;
      }
      inputBits = inputBits.zextOrTrunc(width);
      if (expr.kind == SimExprKind::Ashr && !bits.isZero() &&
          (maxShift >= width ||
           !bits.lshr(width - 1 - maxShift).isZero()))
        inputBits.setBit(width - 1);
      demand(expr.operands[0], inputBits);
      return;
    }
    unsigned shift = static_cast<unsigned>(amount->second.getLimitedValue(width));
    if (expr.kind == SimExprKind::Shl) {
      if (shift < bits.getBitWidth())
        demand(expr.operands[0], bits.lshr(shift));
    } else if (expr.kind == SimExprKind::Lshr) {
      if (shift < width)
        demand(expr.operands[0], bits.zextOrTrunc(width).shl(shift));
    } else {
      llvm::APInt inputBits = shift < width
                                   ? bits.zextOrTrunc(width).shl(shift)
                                   : llvm::APInt(width, 0);
      if (!bits.isZero() &&
          (shift >= width || (shift && !bits.lshr(width - shift).isZero())))
        inputBits.setBit(width - 1);
      demand(expr.operands[0], inputBits);
    }
  } else if (expr.kind == SimExprKind::Mux ||
             expr.kind == SimExprKind::Select) {
    demandFull(expr.operands[0]);
    demand(expr.operands[1], bits);
    demand(expr.operands[2], bits);
  } else {
    for (unsigned input : expr.operands)
      demandFull(input);
  }
}

// Demand flows through the graph's expression DAG until a fixed point. An
// effectful node (state, assignment, instance, or assertion) demands all of
// its operands. Pure comb region yields and arguments preserve exact demand.
static void runGlobalUsedBitsPass(SimGraph &graph) {
  graph.valueUsedBits.clear();
  graph.valueUsedBits.reserve(graph.values.size());
  for (const SimValue &value : graph.values) {
    unsigned width = value.type.shape.empty() && value.type.width
                         ? value.type.width : 1;
    graph.valueUsedBits.push_back(llvm::APInt(width, 0));
  }
  llvm::DenseMap<unsigned, unsigned> producer;
  llvm::DenseMap<unsigned, llvm::APInt> constants;
  for (auto [id, expr] : llvm::enumerate(graph.expressions)) {
    producer.try_emplace(expr.result, static_cast<unsigned>(id));
    if (expr.kind == SimExprKind::Constant)
      constants.try_emplace(expr.result, expr.constant);
  }
  llvm::DenseMap<unsigned, unsigned> regionPassthrough;
  for (const SimCombRegion &region : graph.combRegions) {
    for (auto [result, yielded] : llvm::zip(region.resultIds, region.yieldIds))
      regionPassthrough.try_emplace(result, yielded);
    for (auto [argument, input] : llvm::zip(region.argumentIds, region.inputIds))
      regionPassthrough.try_emplace(argument, input);
  }
  llvm::SmallVector<unsigned> worklist;
  auto demand = [&](unsigned id, const llvm::APInt &mask) {
    llvm::APInt requested = mask.zextOrTrunc(graph.valueUsedBits[id].getBitWidth());
    llvm::APInt joined = graph.valueUsedBits[id] | requested;
    if (joined != graph.valueUsedBits[id]) {
      graph.valueUsedBits[id] = std::move(joined);
      worklist.push_back(id);
    }
  };
  auto demandFull = [&](unsigned id) {
    demand(id, llvm::APInt::getAllOnes(graph.valueUsedBits[id].getBitWidth()));
  };
  for (auto [id, value] : llvm::enumerate(graph.values))
    if (value.observable)
      demandFull(static_cast<unsigned>(id));
  for (const SimNode &node : graph.fineNodes)
    if (node.kind != SimNodeKind::Expression &&
        node.kind != SimNodeKind::CombRegion &&
        node.kind != SimNodeKind::Yield)
      for (unsigned input : node.inputIds)
        demandFull(input);
  while (!worklist.empty()) {
    unsigned result = worklist.pop_back_val();
    if (auto region = regionPassthrough.find(result);
        region != regionPassthrough.end())
      demand(region->second, graph.valueUsedBits[result]);
    auto it = producer.find(result);
    if (it == producer.end())
      continue;
    const SimExpr &expr = graph.expressions[it->second];
    if (graph.values[result].type.shape.empty())
      propagateUsedBits(graph, expr, graph.valueUsedBits[result], constants,
                        demand, demandFull);
    else
      for (unsigned input : expr.operands)
        demandFull(input);
  }
}

// Activity-only usedBits analysis within a pure group. The transfer functions
// operate on captured graph expressions, not on source MLIR operations.
static void runUsedBitActivationPass(SimGraph &graph) {
  llvm::DenseMap<unsigned, llvm::APInt> constants;
  for (const SimExpr &expr : graph.expressions)
    if (expr.kind == SimExprKind::Constant)
      constants.try_emplace(expr.result, expr.constant);
  for (unsigned index : graph.groupedNodes) {
    SimNode &group = graph.topNodes[index];
    if (!group.activateOnInputChange)
      continue;
    llvm::DenseMap<unsigned, unsigned> internalUses;
    for (unsigned expressionId : group.expressionIds)
      for (unsigned operand : graph.expressions[expressionId].operands)
        ++internalUses[operand];
    llvm::DenseMap<unsigned, llvm::APInt> needed;
    bool requiresFullInputs = false;
    auto demand = [&](unsigned value, const llvm::APInt &mask) {
      unsigned width = scalarWidth(graph, value);
      if (!width) {
        requiresFullInputs = true;
        return;
      }
      llvm::APInt clipped = mask.zextOrTrunc(width);
      auto [it, inserted] = needed.try_emplace(value, clipped);
      if (!inserted)
        it->second |= clipped;
    };
    auto demandFull = [&](unsigned value) {
      if (unsigned width = scalarWidth(graph, value))
        demand(value, llvm::APInt::getAllOnes(width));
      else
        requiresFullInputs = true;
    };
    for (unsigned expressionId : group.expressionIds) {
      const SimExpr &expr = graph.expressions[expressionId];
      const SimValue &value = graph.values[expr.result];
      bool visible = value.observable ||
                     value.sourceUseCount > internalUses.lookup(expr.result);
      if (visible)
        demand(expr.result, graph.valueUsedBits[expr.result]);
    }

    for (unsigned expressionId : llvm::reverse(group.expressionIds)) {
      const SimExpr &expr = graph.expressions[expressionId];
      auto found = needed.find(expr.result);
      if (found == needed.end() || found->second.isZero())
        continue;
      propagateUsedBits(graph, expr, found->second, constants, demand,
                        demandFull);
    }
    for (unsigned expressionId : llvm::reverse(group.replicatedExpressionIds)) {
      const SimExpr &expr = graph.expressions[expressionId];
      auto found = needed.find(expr.result);
      if (found != needed.end() && !found->second.isZero())
        propagateUsedBits(graph, expr, found->second, constants, demand,
                          demandFull);
    }
    group.activationUsedBits.clear();
    group.activationUsedBits.reserve(group.inputs.size());
    for (unsigned id : group.inputIds) {
      unsigned width = scalarWidth(graph, id);
      auto it = needed.find(id);
      group.activationUsedBits.push_back(
          width ? (requiresFullInputs ? llvm::APInt::getAllOnes(width)
                                      : (it == needed.end() ? llvm::APInt(width, 0)
                                                            : it->second))
                : llvm::APInt::getAllOnes(1));
    }
    llvm::DenseMap<unsigned, llvm::APInt> vectorMasks;
    llvm::DenseMap<unsigned, llvm::APInt> elementMasks;
    for (unsigned id : group.inputIds) {
      const SimType &type = graph.values[id].type;
      if (!type.shape.empty())
        vectorMasks.try_emplace(id,
                                static_cast<unsigned>(type.shape.front()), 0);
      if (type.shape.size() == 2) {
        uint64_t count = static_cast<uint64_t>(type.shape[0]) *
                         static_cast<uint64_t>(type.shape[1]);
        if (count && count <= std::numeric_limits<unsigned>::max())
          elementMasks.try_emplace(id, static_cast<unsigned>(count), 0);
      }
    }
    llvm::DenseSet<unsigned> seenVectors, fullVectors;
    llvm::DenseSet<unsigned> seenElements, fullElements;
    llvm::DenseSet<unsigned> groupExpressions;
    groupExpressions.insert(group.expressionIds.begin(),
                            group.expressionIds.end());
    groupExpressions.insert(group.replicatedExpressionIds.begin(),
                            group.replicatedExpressionIds.end());
    auto inspectVectorOperands = [&](unsigned expressionId) {
      const SimExpr &expr = graph.expressions[expressionId];
      for (auto [index, operand] : llvm::enumerate(expr.operands)) {
        auto it = vectorMasks.find(operand);
        if (it == vectorMasks.end())
          continue;
        seenVectors.insert(operand);
        if (expr.kind == SimExprKind::VGet && index == 0 &&
            expr.immediate >= 0 &&
            static_cast<uint64_t>(expr.immediate) <
                it->second.getBitWidth())
          it->second.setBit(static_cast<unsigned>(expr.immediate));
        else
          fullVectors.insert(operand);
        auto elements = elementMasks.find(operand);
        if (elements == elementMasks.end())
          continue;
        seenElements.insert(operand);
        const SimType &type = graph.values[operand].type;
        if (expr.kind != SimExprKind::VGet || index != 0 ||
            expr.immediate < 0 || expr.immediate >= type.shape[0]) {
          fullElements.insert(operand);
          continue;
        }
        unsigned row = static_cast<unsigned>(expr.immediate);
        unsigned columns = static_cast<unsigned>(type.shape[1]);
        const SimValue &rowValue = graph.values[expr.result];
        unsigned selectedUses = 0;
        bool wholeRow = rowValue.observable;
        for (unsigned userId : rowValue.expressionUsers) {
          const SimExpr &user = graph.expressions[userId];
          if (!groupExpressions.contains(userId) ||
              user.kind != SimExprKind::VGet ||
              user.operands.size() != 1 ||
              user.operands[0] != expr.result || user.immediate < 0 ||
              static_cast<uint64_t>(user.immediate) >= columns) {
            wholeRow = true;
            break;
          }
          elements->second.setBit(row * columns +
                                  static_cast<unsigned>(user.immediate));
          ++selectedUses;
        }
        if (!selectedUses || selectedUses != rowValue.sourceUseCount)
          wholeRow = true;
        if (wholeRow)
          elements->second.setBits(row * columns, (row + 1) * columns);
      }
    };
    for (unsigned id : group.replicatedExpressionIds)
      inspectVectorOperands(id);
    for (unsigned id : group.expressionIds)
      inspectVectorOperands(id);
    group.activationVectorLanes.clear();
    group.activationVectorLanes.reserve(group.inputIds.size());
    for (unsigned id : group.inputIds) {
      auto it = vectorMasks.find(id);
      if (it == vectorMasks.end()) {
        group.activationVectorLanes.emplace_back(1, 1);
        continue;
      }
      group.activationVectorLanes.push_back(
          fullVectors.contains(id) || !seenVectors.contains(id)
              ? llvm::APInt::getAllOnes(it->second.getBitWidth())
              : it->second);
    }
    group.activationVectorElements.clear();
    group.activationVectorElements.reserve(group.inputIds.size());
    for (unsigned id : group.inputIds) {
      auto it = elementMasks.find(id);
      if (it == elementMasks.end()) {
        group.activationVectorElements.emplace_back(1, 1);
        continue;
      }
      group.activationVectorElements.push_back(
          fullElements.contains(id) || !seenElements.contains(id)
              ? llvm::APInt::getAllOnes(it->second.getBitWidth())
              : it->second);
    }
  }
}

} // namespace

LogicalResult runSimGraphPasses(SimGraph &graph,
                                const SimGraphPassOptions &options) {
  if (failed(graph.verify()))
    return failure();
  auto verifyRewrite = [&]() -> LogicalResult {
    if (failed(graph.rebuildEdges()))
      return failure();
    return graph.verify();
  };
  bool structural = graph.structuralEmission;
  if (!structural && options.enableCombFusion) {
    runConsecutiveCombGroupingPass(graph);
    if (failed(verifyRewrite()))
      return failure();
  }
  if (!structural && options.enableCombFusion) {
    runSharedMuxGroupingPass(graph, options.supernodeMaxSize);
    if (failed(verifyRewrite()))
      return failure();
  }
  if (!structural && options.enableCombFusion) {
    runGraphCoarseningPass(graph);
    if (failed(verifyRewrite()))
      return failure();
  }
  if (!structural && options.enableCombFusion) {
    runSuperNodePartitionPass(graph, options.supernodeMaxSize);
    if (failed(verifyRewrite()))
      return failure();
  }
  if (!structural && options.enableCombFusion) {
    runGlobalInitialPartitionPass(graph, options.supernodeMaxSize);
    if (failed(verifyRewrite()))
      return failure();
  }
  if (!structural && options.enableCombFusion && options.enableExpressionInlining) {
    runExpressionInliningPass(graph);
    if (failed(graph.verify()))
      return failure();
  }
  if (!structural && options.enableCombFusion && options.enableReplication) {
    runReplicationPass(graph);
    if (failed(verifyRewrite()))
      return failure();
  }
  if (!structural && options.enableCombFusion) {
    runMuxConditionBatchPass(graph);
    if (failed(graph.verify()))
      return failure();
  }
  if (!structural && options.enableGroupActivation) {
    runSingletonActivationPass(graph);
    if (failed(graph.verify()))
      return failure();
    runGroupActivationPass(graph);
    if (failed(graph.verify()))
      return failure();
  }
  if (!structural && options.enableUsedBitActivation) {
    runGlobalUsedBitsPass(graph);
    if (failed(graph.verify()))
      return failure();
    runUsedBitActivationPass(graph);
    if (failed(graph.verify()))
      return failure();
  }
  return success();
}

} // namespace pyc
