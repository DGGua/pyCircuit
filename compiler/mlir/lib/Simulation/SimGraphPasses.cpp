#include "pyc/Simulation/SimGraphPasses.h"

#include "pyc/Dialect/PYC/PYCOps.h"

#include "mlir/Dialect/Arith/IR/Arith.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/SmallVector.h"

#include <algorithm>
#include <limits>
#include <set>
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

// Delay condition-controlled nodes while their pure operand producers become
// ready. A batch contains only simultaneously ready nodes, so contracting it
// cannot introduce a quotient cycle. Non-candidates keep deterministic order.
static llvm::SmallVector<llvm::SmallVector<unsigned>> readyConditionSchedule(
    const std::vector<llvm::DenseSet<unsigned>> &predecessors,
    llvm::ArrayRef<unsigned> selectors, unsigned maxBatchSize) {
  const unsigned count = predecessors.size();
  std::vector<llvm::SmallVector<unsigned>> successors(count);
  std::vector<unsigned> remaining(count);
  std::set<unsigned> ready;
  for (unsigned i = 0; i < count; ++i) {
    remaining[i] = predecessors[i].size();
    if (!remaining[i])
      ready.insert(i);
    for (unsigned predecessor : predecessors[i])
      successors[predecessor].push_back(i);
  }
  llvm::SmallVector<llvm::SmallVector<unsigned>> result;
  unsigned scheduled = 0;
  while (!ready.empty()) {
    llvm::SmallVector<unsigned> selected;
    for (unsigned index : ready)
      if (selectors[index] == ~0u) {
        selected.push_back(index);
        break;
      }
    if (selected.empty()) {
      llvm::DenseMap<unsigned, llvm::SmallVector<unsigned>> candidates;
      for (unsigned index : ready)
        if (candidates[selectors[index]].size() < maxBatchSize)
          candidates[selectors[index]].push_back(index);
      for (unsigned index : ready)
        if (candidates[selectors[index]].size() >= 6) {
          selected = candidates[selectors[index]];
          break;
        }
    }
    if (selected.empty())
      selected.push_back(*ready.begin());
    for (unsigned index : selected)
      ready.erase(index);
    for (unsigned index : selected)
      for (unsigned successor : successors[index])
        if (--remaining[successor] == 0)
          ready.insert(successor);
    scheduled += selected.size();
    result.push_back(std::move(selected));
  }
  if (scheduled != count)
    result.clear();
  return result;
}

// Form shared-condition units before coarsening. Pure operands that originally
// followed the first mux can be scheduled first; effectful nodes remain cuts.
static void runSharedMuxGroupingPass(SimGraph &graph, unsigned maxSize) {
  if (maxSize < 6)
    return;
  llvm::DenseSet<unsigned> oldGroups(graph.groupedNodes.begin(),
                                   graph.groupedNodes.end());
  llvm::SmallVector<SimNode, 0> grouped;
  llvm::SmallVector<unsigned> groupIds;
  auto appendOriginal = [&](unsigned index) {
    if (oldGroups.contains(index))
      groupIds.push_back(grouped.size());
    grouped.push_back(std::move(graph.topNodes[index]));
  };
  for (unsigned begin = 0; begin < graph.topNodes.size();) {
    if (!isPureGraphNode(graph.topNodes[begin])) {
      appendOriginal(begin++);
      continue;
    }
    unsigned end = begin + 1;
    while (end < graph.topNodes.size() && isPureGraphNode(graph.topNodes[end]))
      ++end;
    const unsigned count = end - begin;
    llvm::DenseMap<unsigned, unsigned> producer;
    for (unsigned i = 0; i < count; ++i)
      for (unsigned value : graph.topNodes[begin + i].outputIds)
        producer.try_emplace(value, i);
    std::vector<llvm::DenseSet<unsigned>> predecessors(count);
    llvm::SmallVector<unsigned> selectors(count, ~0u);
    for (unsigned i = 0; i < count; ++i) {
      const SimNode &node = graph.topNodes[begin + i];
      for (unsigned value : node.inputIds)
        if (auto it = producer.find(value); it != producer.end() && it->second != i)
          predecessors[i].insert(it->second);
      if (node.kind != SimNodeKind::Expression || node.expressionIds.size() != 1)
        continue;
      const SimExpr &expr = graph.expressions[node.expressionIds.front()];
      if ((expr.kind != SimExprKind::Mux && expr.kind != SimExprKind::Select) ||
          expr.operands.size() != 3 ||
          !graph.values[expr.result].type.shape.empty())
        continue;
      const SimType &selector = graph.values[expr.operands[0]].type;
      if (selector.shape.empty() && selector.width == 1)
        selectors[i] = expr.operands[0];
    }
    auto schedule = readyConditionSchedule(predecessors, selectors, maxSize);
    bool hasBatch = llvm::any_of(schedule, [](const auto &batch) {
      return batch.size() >= 6;
    });
    if (!hasBatch) {
      for (unsigned i = begin; i < end; ++i)
        appendOriginal(i);
    } else {
      for (const auto &batch : schedule) {
        if (batch.size() == 1) {
          appendOriginal(begin + batch.front());
          continue;
        }
        SimNode node;
        node.kind = SimNodeKind::PureGroup;
        for (unsigned index : batch) {
          const SimNode &member = graph.topNodes[begin + index];
          node.operations.append(member.operations.begin(), member.operations.end());
          node.expressionIds.append(member.expressionIds.begin(), member.expressionIds.end());
          node.outputs.append(member.outputs.begin(), member.outputs.end());
          node.outputIds.append(member.outputIds.begin(), member.outputIds.end());
        }
        node.op = node.operations.front();
        rebuildGroupInputs(graph, node);
        groupIds.push_back(grouped.size());
        grouped.push_back(std::move(node));
      }
    }
    begin = end;
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
static void runGraphCoarseningPass(SimGraph &graph, bool strictBound) {
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
          // PYC keeps a state primitive as one node, whereas GSIM separates
          // register source/destination nodes. A path through committed state
          // is not an alternate combinational path between the candidates.
          if (!strictBound && graph.topNodes[current].stateBoundary)
            continue;
          for (unsigned successor : next[current])
            if (seen.insert(successor).second)
              todo.push_back(successor);
        }
        return false;
      };
      llvm::DenseSet<unsigned> used;
      unsigned merged = 0;
      auto tryMerge = [&](unsigned a, unsigned b, unsigned sizeLimit) {
        // GSIM bounds the existing recipient, not the resulting unit. Its
        // out/in passes receive into b; the sibling pass receives into a.
        unsigned recipient = kind == MergeKind::Sibling ? a : b;
        unsigned source = kind == MergeKind::Sibling ? b : a;
        bool exceedsBound = strictBound
                                ? opCount[a] + opCount[b] > sizeLimit
                                : kind == MergeKind::Sibling
                                      ? opCount[recipient] >= 30
                                      : opCount[recipient] > 7000;
        if (a == b || (strictBound && (used.contains(a) || used.contains(b))) ||
            !pure(a) || !pure(b) || exceedsBound ||
            (next[a].contains(b) && next[b].contains(a)) ||
            hasAlternatePath(a, b) || hasAlternatePath(b, a))
          return false;
        if (!strictBound) {
          // Update the quotient immediately so the rest of this single pass
          // sees earlier contractions, as GSIM's mutable supernode graph does.
          parent[source] = recipient;
          opCount[recipient] += opCount[source];
          for (unsigned predecessor : prev[source]) {
            next[predecessor].erase(source);
            if (predecessor != recipient) {
              next[predecessor].insert(recipient);
              prev[recipient].insert(predecessor);
            }
          }
          for (unsigned successor : next[source]) {
            prev[successor].erase(source);
            if (successor != recipient) {
              prev[successor].insert(recipient);
              next[recipient].insert(successor);
            }
          }
          prev[recipient].erase(source);
          next[recipient].erase(source);
          prev[source].clear();
          next[source].clear();
          ++merged;
          return true;
        }
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
      } else if (strictBound) {
        llvm::DenseMap<uint64_t, unsigned> pendingSibling;
        for (unsigned i = 0; i < count; ++i) {
          if (root(i) != i || !pure(i) || prev[i].empty() ||
              (strictBound && opCount[i] >= 30))
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
              if (strictBound)
                pendingSibling.erase(it);
              continue;
            }
          }
          pendingSibling[signature] = i;
        }
      } else {
        // A predecessor hash is only a bucket key: different predecessor sets
        // can have the same sum. Keep all distinct candidates, as GSIM does.
        llvm::DenseMap<uint64_t, llvm::SmallVector<unsigned>> siblingBuckets;
        for (unsigned i = 0; i < count; ++i) {
          if (root(i) != i || !pure(i) || prev[i].empty())
            continue;
          uint64_t signature = prev[i].size();
          for (unsigned predecessor : prev[i])
            signature += (static_cast<uint64_t>(predecessor) + 1) *
                         UINT64_C(0x9e3779b97f4a7c15);
          bool matched = false;
          for (unsigned &candidate : siblingBuckets[signature]) {
            if (prev[candidate].size() != prev[i].size() ||
                !llvm::all_of(prev[i], [&](unsigned p) {
                  return prev[candidate].contains(p);
                }))
              continue;
            matched = true;
            if (!tryMerge(candidate, i, 0))
              candidate = i;
            break;
          }
          if (!matched)
            siblingBuckets[signature].push_back(i);
        }
      }
      if (!merged)
        break;
      changedAny = true;
      // The published source runs each correlation rule once, in order.
      // Repeating to a fixed point can merge additional unrelated siblings.
      if (!strictBound)
        break;
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
// distinct consumer nodes is below its singleton threshold of three. The copies
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
  llvm::DenseMap<unsigned, unsigned> groupByExpression;
  llvm::DenseMap<unsigned, llvm::SmallVector<unsigned, 2>> users;
  llvm::DenseSet<unsigned> requiredMaterialized;
  llvm::DenseSet<unsigned> grouped(graph.groupedNodes.begin(),
                                   graph.groupedNodes.end());
  for (auto [id, expr] : llvm::enumerate(graph.expressions)) {
    producerByValue.try_emplace(expr.result, static_cast<unsigned>(id));
    for (unsigned operand : expr.operands)
      users[operand].push_back(static_cast<unsigned>(id));
  }
  for (auto [index, node] : llvm::enumerate(graph.topNodes)) {
    if (isPureGraphNode(node)) {
      for (unsigned id : node.expressionIds)
        groupByExpression.try_emplace(id, static_cast<unsigned>(index));
      for (unsigned id : node.replicatedExpressionIds)
        groupByExpression.try_emplace(id, static_cast<unsigned>(index));
    } else {
      requiredMaterialized.insert(node.inputIds.begin(), node.inputIds.end());
    }
  }
  for (auto [sourceIndex, source] : llvm::enumerate(graph.topNodes)) {
    if (!isPureGraphNode(source) || source.expressionIds.empty())
      continue;
    // A scalar DAG with one escaping result is one execution expression.
    // Reconstruct its local AST even when the general host inlining policy
    // chose to materialize an internal operation (for example a mux/divide).
    // Groups with multiple escaping roots still contain multiple members.
    llvm::DenseSet<unsigned> localValues;
    for (unsigned id : source.expressionIds)
      localValues.insert(graph.expressions[id].result);
    for (unsigned id : source.replicatedExpressionIds)
      localValues.insert(graph.expressions[id].result);
    unsigned rootId = ~0u;
    bool multipleRoots = false;
    for (unsigned id : source.expressionIds) {
      unsigned valueId = graph.expressions[id].result;
      bool escapes = graph.values[valueId].observable ||
                     requiredMaterialized.contains(valueId);
      for (unsigned userId : users[valueId]) {
        const SimExpr &user = graph.expressions[userId];
        if (!user.omitOriginalEvaluation &&
            llvm::is_contained(user.operands, valueId) &&
            (!groupByExpression.count(userId) ||
             groupByExpression.lookup(userId) != sourceIndex))
          escapes = true;
      }
      if (escapes) {
        multipleRoots |= rootId != ~0u;
        rootId = id;
      }
    }
    if (multipleRoots || rootId == ~0u)
      continue;
    const unsigned rootValue = graph.expressions[rootId].result;
    bool canCopy = true;
    for (unsigned id : localValues) {
      const SimValue &value = graph.values[id];
      if (value.observable || requiredMaterialized.contains(id) ||
          !value.type.shape.empty() || value.type.width > 64) {
        canCopy = false;
        break;
      }
    }
    if (!canCopy)
      continue;
    llvm::DenseMap<unsigned, int> costs;
    std::function<int(unsigned)> expressionCost = [&](unsigned id) -> int {
      if (!localValues.contains(id)) {
        auto producer = producerByValue.find(id);
        // A materialized value is a leaf, even if it is a wide integer or
        // an array. The copied operation must still have a scalar result of
        // at most BASIC_WIDTH. Aggregate construction is never copied.
        return graph.values[id].sourceBacked &&
                       (producer == producerByValue.end() ||
                        !graph.expressions[producer->second].omitOriginalEvaluation)
                   ? 0 : -1;
      }
      if (auto it = costs.find(id); it != costs.end())
        return it->second;
      const SimExpr &expr = graph.expressions[producerByValue.lookup(id)];
      int cost = replicationOpCost(expr.kind);
      if (cost < 0)
        return -1;
      for (unsigned operand : expr.operands) {
        int child = expressionCost(operand);
        if (child < 0)
          return -1;
        // Count repeated DAG references as repeated inline evaluation, and
        // saturate at the strict GSIM threshold rather than overflowing.
        cost = std::min(3, cost + child);
      }
      costs.try_emplace(id, cost);
      return cost;
    };
    const int cost = expressionCost(rootValue);
    if (cost < 0 || cost >= 3)
      continue;
    llvm::SmallVector<std::pair<unsigned, unsigned>> consumers;
    llvm::DenseSet<unsigned> distinctUsers;
    for (unsigned userId : users[rootValue]) {
      const SimExpr &user = graph.expressions[userId];
      // The use list also contains old source expressions retained for
      // diagnostics and operands redirected by previous copies.
      if (user.omitOriginalEvaluation ||
          !llvm::is_contained(user.operands, rootValue) ||
          !distinctUsers.insert(userId).second)
        continue;
      auto it = groupByExpression.find(userId);
      if (it == groupByExpression.end() || it->second == sourceIndex) {
        canCopy = false;
        break;
      }
      consumers.emplace_back(it->second, userId);
    }
    if (!canCopy || consumers.empty() ||
        (cost && distinctUsers.size() >= (3u + cost - 1u) / cost))
      continue;
    // No internal source value may escape with the removed execution node.
    for (unsigned id : localValues) {
      if (id == rootValue)
        continue;
      for (unsigned userId : users[id]) {
        const SimExpr &user = graph.expressions[userId];
        if (!user.omitOriginalEvaluation &&
            llvm::is_contained(user.operands, id) &&
            (!groupByExpression.count(userId) ||
             groupByExpression.lookup(userId) != sourceIndex))
          canCopy = false;
      }
    }
    if (!canCopy)
      continue;
    llvm::SmallVector<unsigned> orderedGroups;
    for (auto [groupIndex, consumerId] : consumers) {
      (void)consumerId;
      if (!llvm::is_contained(orderedGroups, groupIndex))
        orderedGroups.push_back(groupIndex);
    }
    for (unsigned groupIndex : orderedGroups) {
      llvm::DenseMap<unsigned, unsigned> copiedValues;
      llvm::SmallVector<unsigned> copiedExpressions;
      std::function<unsigned(unsigned)> copyValue = [&](unsigned id) -> unsigned {
        if (!localValues.contains(id))
          return id;
        if (auto it = copiedValues.find(id); it != copiedValues.end())
          return it->second;
        SimExpr copy = graph.expressions[producerByValue.lookup(id)];
        for (unsigned &operand : copy.operands)
          operand = copyValue(operand);
        unsigned newValue = graph.values.size();
        const SimType type = graph.values[id].type;
        graph.values.push_back(SimValue{type, Value{}, 0, false});
        copy.source = nullptr;
        copy.result = newValue;
        copy.inlineIntoConsumer = true;
        copy.omitOriginalEvaluation = false;
        unsigned newExpression = graph.expressions.size();
        graph.expressions.push_back(std::move(copy));
        for (unsigned operand : graph.expressions.back().operands)
          users[operand].push_back(newExpression);
        producerByValue.try_emplace(newValue, newExpression);
        groupByExpression.try_emplace(newExpression, groupIndex);
        copiedExpressions.push_back(newExpression);
        copiedValues.try_emplace(id, newValue);
        return newValue;
      };
      unsigned cloneValue = copyValue(rootValue);
      auto &replicas = graph.topNodes[groupIndex].replicatedExpressionIds;
      replicas.insert(replicas.begin(), copiedExpressions.begin(),
                      copiedExpressions.end());
      for (auto [consumerGroup, consumerId] : consumers) {
        if (consumerGroup != groupIndex)
          continue;
        for (unsigned &operand : graph.expressions[consumerId].operands)
          if (operand == rootValue) {
            operand = cloneValue;
            users[cloneValue].push_back(consumerId);
          }
      }
      if (grouped.insert(groupIndex).second)
        graph.groupedNodes.push_back(groupIndex);
      rebuildGroupInputs(graph, graph.topNodes[groupIndex]);
    }
    for (unsigned id : source.expressionIds)
      graph.expressions[id].omitOriginalEvaluation = true;
    for (unsigned id : source.replicatedExpressionIds)
      graph.expressions[id].omitOriginalEvaluation = true;
  }
  llvm::SmallVector<SimNode, 0> kept;
  llvm::SmallVector<unsigned> remap(graph.topNodes.size(), ~0u);
  for (auto [index, node] : llvm::enumerate(graph.topNodes)) {
    if (!node.expressionIds.empty() &&
        llvm::all_of(node.expressionIds, [&](unsigned id) {
          return graph.expressions[id].omitOriginalEvaluation;
        }))
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

// Schedule the pure expression DAG before forming common-selector statements.
// Inlined branch expressions remain graph expressions and are rendered inside
// the selected branch. The selector itself stays materialized.
static void runMuxConditionBatchPass(SimGraph &graph) {
  llvm::DenseSet<unsigned> inlinedValues;
  for (const SimExpr &expr : graph.expressions)
    if (expr.inlineIntoConsumer)
      inlinedValues.insert(expr.result);
  llvm::DenseMap<Operation *, unsigned> sourceSegments;
  unsigned segment = 0;
  for (const SimNode &node : graph.fineNodes) {
    if (isPureGraphNode(node))
      sourceSegments.try_emplace(node.op, segment);
    else
      ++segment;
  }
  for (unsigned groupIndex : graph.groupedNodes) {
    SimNode &group = graph.topNodes[groupIndex];
    group.muxConditionBatches.clear();
    unsigned count = group.expressionIds.size();
    llvm::DenseMap<unsigned, unsigned> producer;
    for (unsigned i = 0; i < count; ++i)
      producer.try_emplace(graph.expressions[group.expressionIds[i]].result, i);
    std::vector<llvm::DenseSet<unsigned>> predecessors(count);
    llvm::SmallVector<unsigned> selectors(count, ~0u);
    llvm::DenseMap<std::pair<unsigned, unsigned>, unsigned> conditionKeys;
    for (unsigned i = 0; i < count; ++i) {
      const SimExpr &expr = graph.expressions[group.expressionIds[i]];
      for (unsigned operand : expr.operands)
        if (auto it = producer.find(operand); it != producer.end())
          predecessors[i].insert(it->second);
      if ((expr.kind != SimExprKind::Mux && expr.kind != SimExprKind::Select) ||
          expr.operands.size() != 3 || expr.inlineIntoConsumer ||
          expr.omitOriginalEvaluation ||
          !graph.values[expr.result].type.shape.empty() ||
          !graph.values[expr.result].sourceBacked)
        continue;
      unsigned selectorId = expr.operands[0];
      const SimValue &selector = graph.values[selectorId];
      if (selector.type.shape.empty() && selector.type.width == 1 &&
          selector.sourceBacked && !inlinedValues.contains(selectorId)) {
        auto key = std::make_pair(sourceSegments.lookup(expr.source), selectorId);
        auto [it, inserted] = conditionKeys.try_emplace(key, conditionKeys.size());
        (void)inserted;
        selectors[i] = it->second;
      }
    }
    auto schedule = readyConditionSchedule(predecessors, selectors, count);
    if (!llvm::any_of(schedule, [](const auto &batch) { return batch.size() >= 6; }))
      continue;
    llvm::SmallVector<unsigned> order;
    for (const auto &batch : schedule) {
      unsigned begin = order.size();
      order.append(batch.begin(), batch.end());
      if (batch.size() >= 6)
        group.muxConditionBatches.push_back(
            {begin, static_cast<unsigned>(order.size()),
             graph.expressions[group.expressionIds[batch.front()]].operands[0]});
    }
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
                              llvm::APInt bits,
                              const llvm::DenseMap<unsigned, llvm::APInt> &constants,
                              Demand demand,
                              DemandFull demandFull) {
  // `bits` is taken by value. Callers hand over a mask stored in a DenseMap,
  // and demand() may insert into that map and rehash it before this function
  // reads the mask again.
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
  if (!structural && options.enableCombFusion && options.strictSupernodeBound) {
    runConsecutiveCombGroupingPass(graph);
    if (failed(verifyRewrite()))
      return failure();
  }
  if (!structural && options.enableCombFusion) {
    runSharedMuxGroupingPass(graph, options.strictSupernodeBound
                                       ? options.supernodeMaxSize
                                       : std::numeric_limits<unsigned>::max());
    if (failed(verifyRewrite()))
      return failure();
  }
  if (!structural && options.enableCombFusion) {
    runGraphCoarseningPass(graph, options.strictSupernodeBound);
    if (failed(verifyRewrite()))
      return failure();
  }
  if (!structural && options.enableCombFusion && options.strictSupernodeBound) {
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
