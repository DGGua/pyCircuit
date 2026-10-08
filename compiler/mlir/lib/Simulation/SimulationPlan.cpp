#include "pyc/Simulation/SimulationPlan.h"

#include "pyc/Dialect/PYC/PYCOps.h"
#include "pyc/Dialect/PYC/PYCTypes.h"

#include "mlir/Dialect/Arith/IR/Arith.h"
#include "mlir/Dialect/Func/IR/FuncOps.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/SmallSet.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/ADT/STLExtras.h"

#include <algorithm>
#include <functional>
#include <iterator>
#include <limits>
#include <map>
#include <queue>
#include <set>
#include <string>
#include <tuple>
#include <vector>

using namespace mlir;

namespace pyc {
namespace {

static std::string sanitizeId(llvm::StringRef s) {
  std::string out;
  out.reserve(s.size() + 1);
  for (char c : s)
    out.push_back(((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                   (c >= '0' && c <= '9') || c == '_') ? c : '_');
  if (out.empty() || (out.front() >= '0' && out.front() <= '9'))
    out.insert(out.begin(), '_');
  return out;
}

static InstanceInterfacePlan planInstanceInterface(const SimNode &node) {
  InstanceInterfacePlan result;
  llvm::StringMap<unsigned> used;
  auto unique = [&](std::string base) {
    unsigned &count = used[base];
    return ++count == 1 ? base : base + "_" + std::to_string(count);
  };
  for (const std::string &path : node.instanceInputPortPaths)
    result.inputPorts.push_back(unique(sanitizeId(path)));
  for (const std::string &path : node.instanceOutputPortPaths)
    result.outputPorts.push_back(unique(sanitizeId(path)));
  return result;
}

static bool supportsScalarFingerprint(const SimType &type) {
  return type.shape.empty() && type.width <= 64;
}

// appendPackedWireWords visits every vector element recursively and writes
// ceil(element width / 64) words for each one, including partial words.
static FailureOr<unsigned> instanceCacheWordCount(const SimGraph &graph,
                                                 const SimNode &node) {
  uint64_t total = 0;
  constexpr uint64_t limit = std::numeric_limits<unsigned>::max();
  for (unsigned inputId : node.inputIds) {
    const SimType &type = graph.values[inputId].type;
    uint64_t words = std::max<uint64_t>(
        1, (static_cast<uint64_t>(type.width) + 63) / 64);
    for (int64_t dimension : type.shape) {
      if (dimension <= 0 || static_cast<uint64_t>(dimension) > limit / words)
        return failure();
      words *= static_cast<uint64_t>(dimension);
    }
    if (words > limit - total)
      return failure();
    total += words;
  }
  return static_cast<unsigned>(total);
}

// Match the emitter's existing deterministic node-key allocation. This is
// only a tie-break for otherwise independent nodes, not a dependency source.
struct NodeKeys {
  const SimGraph &graph;
  llvm::DenseMap<unsigned, std::string> names;
  llvm::StringMap<unsigned> used;
  int next = 0;

  std::string unique(std::string base) {
    unsigned &n = used[base];
    return ++n == 1 ? base : base + "_" + std::to_string(n);
  }
  std::string get(unsigned id) {
    if (auto it = names.find(id); it != names.end())
      return it->second;
    if (id >= graph.values.size())
      llvm::report_fatal_error("simulation plan: name requested for missing graph value");
    const SimValue &value = graph.values[id];
    if (value.sourceNameIsExplicit)
      return names.try_emplace(id, unique(sanitizeId(value.sourceNameBase))).first->second;
    if (!value.sourceNameBase.empty()) {
      std::string base = sanitizeId(value.sourceNameBase);
      if (base.empty())
        base = "v";
      return names.try_emplace(id, unique(base + "_" + std::to_string(++next))).first->second;
    }
    return names.try_emplace(id, unique("arg_" + std::to_string(++next))).first->second;
  }
  explicit NodeKeys(const SimGraph &graph) : graph(graph) {
    for (auto [i, id] : llvm::enumerate(graph.inputValueIds))
      names.try_emplace(id,
                        unique(sanitizeId(graph.inputPortPaths[i])));
    for (const std::string &path : graph.outputPortPaths)
      (void)unique(sanitizeId(path));
    for (const SimNode &node : graph.fineNodes)
      for (unsigned id : node.outputIds)
        (void)get(id);
  }
};

static bool includeForEval(const SimNode &node, bool includePrimitives) {
  if (node.kind == SimNodeKind::Return || node.kind == SimNodeKind::Wire ||
      node.kind == SimNodeKind::Reg || node.kind == SimNodeKind::SyncMem ||
      node.kind == SimNodeKind::SyncMemDP ||
      node.kind == SimNodeKind::CdcSync)
    return false;
  if (!includePrimitives &&
      (node.kind == SimNodeKind::Fifo ||
       node.kind == SimNodeKind::AsyncFifo ||
       node.kind == SimNodeKind::ByteMem ||
       node.kind == SimNodeKind::Instance))
    return false;
  return true;
}

struct DependencyGraph {
  llvm::SmallVector<const SimNode *> nodes;
  llvm::SmallVector<unsigned> topNodeIndices;
  llvm::SmallVector<std::string> keys;
  llvm::SmallVector<llvm::SmallVector<unsigned>> succ;
  llvm::SmallVector<unsigned> indeg;
};

static bool buildDependencies(const SimGraph &graph, NodeKeys &names,
                              bool includePrimitives, DependencyGraph &out) {
  if (graph.hasAmbiguousDrivers)
    return false;
  std::vector<int> topToEval(graph.topNodes.size(), -1);
  for (auto [topIndex, node] : llvm::enumerate(graph.topNodes)) {
    if (!includeForEval(node, includePrimitives))
      continue;
    unsigned idx = static_cast<unsigned>(out.nodes.size());
    topToEval[topIndex] = static_cast<int>(idx);
    out.nodes.push_back(&node);
    out.topNodeIndices.push_back(static_cast<unsigned>(topIndex));
    if (node.kind == SimNodeKind::Assign)
      out.keys.push_back(names.get(node.assignedValueId));
    else if (!node.outputIds.empty())
      out.keys.push_back(names.get(node.outputIds.front()));
    else
      out.keys.push_back(sanitizeId(node.sourceOperationName) + "_" + std::to_string(idx));
  }

  out.succ.resize(out.nodes.size());
  out.indeg.resize(out.nodes.size());
  llvm::SmallVector<llvm::SmallSet<unsigned, 8>> deps(out.nodes.size());
  for (const SimEdge &edge : graph.edges) {
    int from = topToEval[edge.producer];
    int to = topToEval[edge.consumer];
    if (from >= 0 && to >= 0 && from != to)
      deps[to].insert(static_cast<unsigned>(from));
  }
  for (unsigned i = 0; i < out.nodes.size(); ++i) {
    out.indeg[i] = deps[i].size();
    for (unsigned p : deps[i])
      out.succ[p].push_back(static_cast<unsigned>(i));
  }
  return true;
}

static bool topoOrder(const DependencyGraph &graph,
                      llvm::SmallVector<unsigned> &orderedIds) {
  llvm::SmallVector<unsigned> indeg = graph.indeg;
  auto cmp = [&](unsigned a, unsigned b) { return graph.keys[a] > graph.keys[b]; };
  std::vector<unsigned> heap;
  for (unsigned i = 0; i < graph.nodes.size(); ++i)
    if (indeg[i] == 0)
      heap.push_back(i);
  std::make_heap(heap.begin(), heap.end(), cmp);
  while (!heap.empty()) {
    std::pop_heap(heap.begin(), heap.end(), cmp);
    unsigned n = heap.back();
    heap.pop_back();
    orderedIds.push_back(graph.topNodeIndices[n]);
    for (unsigned s : graph.succ[n])
      if (--indeg[s] == 0) {
        heap.push_back(s);
        std::push_heap(heap.begin(), heap.end(), cmp);
      }
  }
  if (orderedIds.size() == graph.nodes.size())
    return true;
  orderedIds.clear();
  return false;
}

static void collectOperationOrder(const SimGraph &graph, NodeKeys &names,
                                  OperationOrder &order) {
  for (auto [id, node] : llvm::enumerate(graph.topNodes)) {
    unsigned nodeId = static_cast<unsigned>(id);
    switch (node.kind) {
    case SimNodeKind::Reg: order.regs.push_back(nodeId); break;
    case SimNodeKind::Fifo: order.fifos.push_back(nodeId); break;
    case SimNodeKind::ByteMem: order.byteMems.push_back(nodeId); break;
    case SimNodeKind::SyncMem: order.syncMems.push_back(nodeId); break;
    case SimNodeKind::SyncMemDP: order.syncMemDPs.push_back(nodeId); break;
    case SimNodeKind::AsyncFifo: order.asyncFifos.push_back(nodeId); break;
    case SimNodeKind::CdcSync: order.cdcSyncs.push_back(nodeId); break;
    case SimNodeKind::Instance: order.instances.push_back(nodeId); break;
    case SimNodeKind::CombRegion: order.combs.push_back(nodeId); break;
    default: break;
    }
  }
  auto sortBy = [&](auto &ops, auto key) {
    std::sort(ops.begin(), ops.end(), [&](unsigned a, unsigned b) {
      return key(a) < key(b);
    });
  };
  sortBy(order.regs, [&](unsigned id) {
    const SimNode &reg = graph.topNodes[id];
    return names.get(reg.outputIds.front());
  });
  sortBy(order.fifos, [&](unsigned id) {
    const SimNode &fifo = graph.topNodes[id];
    return names.get(fifo.outputIds.front());
  });
  auto memName = [&](unsigned id) {
    const SimNode &mem = graph.topNodes[id];
    if (mem.primitiveHasName)
      return sanitizeId(mem.primitiveName);
    return names.get(mem.outputIds.front());
  };
  sortBy(order.byteMems, memName);
  sortBy(order.syncMems, memName);
  sortBy(order.syncMemDPs, memName);
  sortBy(order.asyncFifos, [&](unsigned id) {
    const SimNode &fifo = graph.topNodes[id];
    return names.get(fifo.outputIds.front());
  });
  sortBy(order.cdcSyncs, [&](unsigned id) {
    const SimNode &cdc = graph.topNodes[id];
    return names.get(cdc.outputIds.front());
  });
  sortBy(order.combs, [&](unsigned id) {
    return names.get(graph.topNodes[id].outputIds.front());
  });
  sortBy(order.instances, [&](unsigned id) {
    const SimNode &node = graph.topNodes[id];
    if (node.instanceHasName)
      return sanitizeId(node.instanceName);
    if (!node.outputIds.empty())
      return names.get(node.outputIds.front());
    return std::string("inst");
  });
}

static void buildSccOrder(const DependencyGraph &graph, SimulationPlan &plan) {
  if (graph.nodes.empty())
    return;
  std::vector<int> index(graph.nodes.size(), -1), low(graph.nodes.size(), 0);
  std::vector<unsigned> stack;
  std::vector<bool> inStack(graph.nodes.size(), false);
  llvm::SmallVector<unsigned> nodeToComp(graph.nodes.size(), 0);
  struct Component {
    llvm::SmallVector<unsigned> nodes;
    llvm::SmallVector<unsigned> succ;
    unsigned indeg = 0;
    std::string key;
    bool cyclic = false;
  };
  llvm::SmallVector<Component> comps;
  int nextIndex = 0;
  std::function<void(unsigned)> strongconnect = [&](unsigned v) {
    index[v] = low[v] = nextIndex++;
    stack.push_back(v);
    inStack[v] = true;
    for (unsigned w : graph.succ[v]) {
      if (index[w] < 0) {
        strongconnect(w);
        low[v] = std::min(low[v], low[w]);
      } else if (inStack[w]) {
        low[v] = std::min(low[v], index[w]);
      }
    }
    if (low[v] != index[v])
      return;
    Component comp;
    while (!stack.empty()) {
      unsigned w = stack.back();
      stack.pop_back();
      inStack[w] = false;
      nodeToComp[w] = comps.size();
      comp.nodes.push_back(w);
      if (w == v)
        break;
    }
    std::sort(comp.nodes.begin(), comp.nodes.end(),
              [&](unsigned a, unsigned b) { return graph.keys[a] < graph.keys[b]; });
    comp.key = graph.keys[comp.nodes.front()];
    comps.push_back(std::move(comp));
  };
  for (unsigned v = 0; v < graph.nodes.size(); ++v)
    if (index[v] < 0)
      strongconnect(v);

  llvm::SmallVector<llvm::SmallSet<unsigned, 8>> succSets(comps.size());
  for (unsigned v = 0; v < graph.nodes.size(); ++v) {
    unsigned cv = nodeToComp[v];
    for (unsigned w : graph.succ[v]) {
      unsigned cw = nodeToComp[w];
      if (cv == cw) {
        if (v == w || comps[cv].nodes.size() > 1)
          comps[cv].cyclic = true;
      } else {
        succSets[cv].insert(cw);
      }
    }
    if (comps[cv].nodes.size() > 1)
      comps[cv].cyclic = true;
  }
  for (unsigned c = 0; c < comps.size(); ++c) {
    for (unsigned s : succSets[c]) {
      comps[c].succ.push_back(s);
      ++comps[s].indeg;
    }
    std::sort(comps[c].succ.begin(), comps[c].succ.end(),
              [&](unsigned a, unsigned b) { return comps[a].key < comps[b].key; });
  }

  auto cmp = [&](unsigned a, unsigned b) { return comps[a].key > comps[b].key; };
  std::vector<unsigned> heap;
  for (unsigned i = 0; i < comps.size(); ++i)
    if (comps[i].indeg == 0)
      heap.push_back(i);
  std::make_heap(heap.begin(), heap.end(), cmp);
  while (!heap.empty()) {
    std::pop_heap(heap.begin(), heap.end(), cmp);
    unsigned c = heap.back();
    heap.pop_back();
    ScheduledComponent scheduled;
    scheduled.cyclic = comps[c].cyclic;
    for (unsigned n : comps[c].nodes)
      scheduled.nodeIds.push_back(graph.topNodeIndices[n]);
    scheduled.iterationLimit = std::max(2u, plan.primitiveCount +
                                             static_cast<unsigned>(scheduled.nodeIds.size()) + 2u);
    plan.useSccWorklist |= scheduled.cyclic;
    plan.sccOrder.push_back(std::move(scheduled));
    for (unsigned s : comps[c].succ)
      if (--comps[s].indeg == 0) {
        heap.push_back(s);
        std::push_heap(heap.begin(), heap.end(), cmp);
      }
  }
  if (plan.sccOrder.size() != comps.size()) {
    plan.sccOrder.clear();
    plan.useSccWorklist = false;
  }
}

static bool hasCommitOnlyOutputs(const SimNode &node) {
  return node.kind == SimNodeKind::Reg || node.kind == SimNodeKind::SyncMem ||
         node.kind == SimNodeKind::SyncMemDP || node.kind == SimNodeKind::CdcSync;
}

static bool publishesOnCommit(const SimNode &node) {
  // Async FIFO and byte-memory commit methods also refresh their public read
  // outputs. A later eval snapshot alone would miss those changes.
  return hasCommitOnlyOutputs(node) || node.kind == SimNodeKind::ByteMem ||
         node.kind == SimNodeKind::AsyncFifo;
}

static bool canPublishActivity(const SimulationPlan &plan, unsigned producer,
                               unsigned target,
                               const llvm::DenseMap<unsigned, unsigned> &combPosition,
                               const llvm::DenseMap<unsigned, unsigned> &evalPosition) {
  const SimNode &node = plan.graph.topNodes[producer];
  if (hasCommitOnlyOutputs(node))
    return true;
  switch (plan.nodeActions[producer].kind) {
  case SimNodeActionKind::Group:
  case SimNodeActionKind::Expression:
  case SimNodeActionKind::Assign:
  case SimNodeActionKind::CombRegion:
    return combPosition.count(producer) && combPosition.count(target) &&
           combPosition.lookup(producer) < combPosition.lookup(target);
  case SimNodeActionKind::Fifo:
  case SimNodeActionKind::AsyncFifo:
  case SimNodeActionKind::ByteMem:
  case SimNodeActionKind::Instance:
    // Hierarchical state feedback may require repeated SCC evaluation. Keep
    // its existing cache protocol until a complete publication order exists.
    return plan.evalTopological && evalPosition.count(producer) &&
           evalPosition.count(target) &&
           evalPosition.lookup(producer) < evalPosition.lookup(target);
  default:
    return false;
  }
}

static void planPackedGroupActivation(SimulationPlan &plan) {
  if (!plan.combTopological || plan.graph.hasAmbiguousDrivers)
    return;
  llvm::DenseMap<unsigned, unsigned> position;
  for (auto [order, id] : llvm::enumerate(plan.combNodeOrder))
    position.try_emplace(id, static_cast<unsigned>(order));
  llvm::DenseMap<unsigned, unsigned> evalPosition;
  for (auto [order, id] : llvm::enumerate(plan.evalNodeOrder))
    evalPosition.try_emplace(id, static_cast<unsigned>(order));
  llvm::DenseMap<unsigned, llvm::DenseMap<unsigned, unsigned>> inputProducer;
  for (const SimEdge &edge : plan.graph.edges)
    if (plan.groupActivations.count(edge.consumer) &&
        canPublishActivity(plan, edge.producer, edge.consumer, position,
                           evalPosition))
      inputProducer[edge.consumer].try_emplace(edge.valueId, edge.producer);
  for (unsigned target : plan.graph.groupedNodes) {
    if (!plan.groupActivations.count(target))
      continue;
    bool allPublished = true;
    for (unsigned value : plan.graph.topNodes[target].inputIds) {
      auto it = inputProducer[target].find(value);
      if (it == inputProducer[target].end()) {
        allPublished = false;
        break;
      }
    }
    if (allPublished)
      plan.packedActivationGroups.insert(target);
  }
  std::map<std::pair<unsigned, unsigned>, std::set<unsigned>> propagation;
  for (const SimEdge &edge : plan.graph.edges)
    if (plan.packedActivationGroups.contains(edge.consumer))
      propagation[{edge.producer, edge.consumer}].insert(edge.valueId);
  for (const auto &[pair, values] : propagation) {
    GroupPropagationTarget target;
    target.groupNodeId = pair.second;
    const GroupActivationPlan &activation = plan.groupActivations.lookup(pair.second);
    for (unsigned value : values) {
      target.valueIds.push_back(value);
      auto input = llvm::find(activation.inputIds, value);
      unsigned index = static_cast<unsigned>(input - activation.inputIds.begin());
      if (index < activation.usedBits.size())
        target.usedBits.push_back(activation.usedBits[index]);
      else {
        const SimType &type = plan.graph.values[value].type;
        target.usedBits.emplace_back(
            type.shape.empty() && type.width ? type.width : 1, 0, true);
      }
      if (index < activation.vectorLanes.size())
        target.vectorLanes.push_back(activation.vectorLanes[index]);
      else {
        const SimType &type = plan.graph.values[value].type;
        target.vectorLanes.push_back(llvm::APInt::getAllOnes(
            type.shape.empty() ? 1u
                               : static_cast<unsigned>(type.shape.front())));
      }
      if (index < activation.vectorElements.size())
        target.vectorElements.push_back(activation.vectorElements[index]);
      else
        target.vectorElements.emplace_back(1, 1);
    }
    plan.groupPropagations[pair.first].push_back(std::move(target));
    if (publishesOnCommit(plan.graph.topNodes[pair.first]))
      plan.commitPropagationNodes.insert(pair.first);
  }
}

static bool sameActivityPredicate(const GroupPropagationTarget &a,
                                  const GroupPropagationTarget &b) {
  return a.valueIds == b.valueIds && a.usedBits == b.usedBits &&
         a.vectorLanes == b.vectorLanes && a.vectorElements == b.vectorElements;
}

static void planActivityPublications(SimulationPlan &plan) {
  for (const auto &[source, targets] : plan.groupPropagations) {
    auto &publications = plan.activityPublications[source];
    for (const GroupPropagationTarget &target : targets) {
      auto it = llvm::find_if(publications, [&](const ActivityPublication &pub) {
        return sameActivityPredicate(pub.predicate, target);
      });
      if (it == publications.end()) {
        publications.emplace_back();
        it = publications.end() - 1;
        it->predicate = target;
      }
      unsigned group = plan.groupByNode.lookup(target.groupNodeId);
      unsigned word = group / 64u;
      auto store = llvm::find_if(it->stores, [&](const ActivityWordMask &s) {
        return s.word == word;
      });
      if (store == it->stores.end())
        it->stores.push_back(ActivityWordMask{word, uint64_t{1} << (group % 64u)});
      else
        store->mask |= uint64_t{1} << (group % 64u);
    }
    // GSIM selects masked stores for at most three bitmap updates and one
    // common branch for larger fanout. PYC uses aligned uint64_t words.
    for (ActivityPublication &pub : publications)
      pub.branchless = pub.stores.size() <= 3;
  }
}

static llvm::SmallVector<ActivityBatch>
planActivityBatches(const SimulationPlan &plan, llvm::ArrayRef<unsigned> nodes) {
  llvm::SmallVector<ActivityBatch> batches;
  for (unsigned node : nodes) {
    if (!plan.packedActivationGroups.contains(node)) {
      batches.push_back(ActivityBatch{{node}, 0, 0});
      continue;
    }
    unsigned group = plan.groupByNode.lookup(node);
    unsigned word = group / 64u;
    // Limit outer checks to eight adjacent groups, as in the source byte
    // block check, without aliasing wide stores through smaller integers.
    unsigned byte = group / 8u;
    if (batches.empty() || !batches.back().mask ||
        plan.groupByNode.lookup(batches.back().nodeIds.front()) / 8u != byte)
      batches.push_back(ActivityBatch{{}, word, 0});
    batches.back().nodeIds.push_back(node);
    batches.back().mask |= uint64_t{1} << (group % 64u);
  }
  return batches;
}

// Adapt the donor's locality schedule and weighted cuts to graph expression
// IDs. This only changes a pure method's internal order; SimGraph groups and
// their activation/topological schedule remain intact. Existing mux batches
// and nested comb steps retain their original schedules.
struct PureMethodSchedule {
  llvm::SmallVector<unsigned> expressions;
  llvm::SmallVector<unsigned> ends;
};

static PureMethodSchedule planPureMethods(const SimGraph &graph,
                                           llvm::ArrayRef<unsigned> original,
                                           unsigned limit) {
  PureMethodSchedule fixed;
  fixed.expressions.assign(original.begin(), original.end());
  for (unsigned begin = 0; begin < original.size(); begin += limit)
    fixed.ends.push_back(std::min<unsigned>(original.size(), begin + limit));
  if (original.size() <= limit)
    return fixed;
  const unsigned n = original.size();
  llvm::DenseMap<unsigned, unsigned> producer;
  for (auto [index, id] : llvm::enumerate(original))
    producer.try_emplace(graph.expressions[id].result, index);
  llvm::SmallVector<llvm::SmallVector<unsigned>> successors(n);
  llvm::SmallVector<unsigned> indegree(n, 0), remainingUses(n, 0);
  llvm::SmallVector<uint64_t> weight(n, 0), work(n, 1);
  llvm::SmallVector<unsigned> depth(n, 1);
  constexpr uint64_t inf = std::numeric_limits<uint64_t>::max() / 4;
  for (auto [index, id] : llvm::enumerate(original)) {
    llvm::SmallSet<unsigned, 4> dependencies;
    for (unsigned input : graph.expressions[id].operands)
      if (auto found = producer.find(input); found != producer.end()) {
        dependencies.insert(found->second);
        ++remainingUses[found->second];
      }
    indegree[index] = dependencies.size();
    for (unsigned dependency : dependencies)
      successors[dependency].push_back(index);
  }
  for (unsigned i = 0; i < n; ++i) {
    const SimExpr &expr = graph.expressions[original[i]];
    const SimValue &value = graph.values[expr.result];
    // External/observable wires cannot benefit from method-local storage.
    if (value.observable || value.sourceNameIsExplicit || expr.inlineIntoConsumer ||
        expr.omitOriginalEvaluation || value.sourceUseCount != remainingUses[i])
      continue;
    uint64_t w = 1 + (static_cast<uint64_t>(value.type.width) + 63) / 64;
    for (int64_t d : value.type.shape)
      w = d > 0 && static_cast<uint64_t>(d) <= inf / w ? w * d : inf;
    weight[i] = w;
  }
  for (unsigned i = n; i-- > 0;)
    for (unsigned next : successors[i]) {
      work[i] = std::min(inf, work[i] + work[next]);
      depth[i] = std::max(depth[i], depth[next] + 1);
    }
  struct Ready {
    uint64_t work;
    unsigned depth;
    int64_t delta;
    uint64_t closed;
    unsigned index;
  };
  auto worse = [](const Ready &a, const Ready &b) {
    return std::tie(a.work, a.depth, a.delta) != std::tie(b.work, b.depth, b.delta)
               ? std::tie(a.work, a.depth, a.delta) > std::tie(b.work, b.depth, b.delta)
               : a.closed != b.closed ? a.closed < b.closed : a.index > b.index;
  };
  std::priority_queue<Ready, std::vector<Ready>, decltype(worse)> ready(worse);
  auto enqueue = [&](unsigned index) {
    uint64_t closed = 0;
    llvm::DenseMap<unsigned, unsigned> consumed;
    for (unsigned input : graph.expressions[original[index]].operands)
      if (auto found = producer.find(input); found != producer.end())
        ++consumed[found->second];
    for (const auto &entry : consumed)
      if (remainingUses[entry.first] == entry.second)
        closed = std::min(inf, closed + weight[entry.first]);
    ready.push({work[index], depth[index],
                static_cast<int64_t>(weight[index]) - static_cast<int64_t>(closed),
                closed, index});
  };
  for (unsigned i = 0; i < n; ++i)
    if (indegree[i] == 0)
      enqueue(i);
  PureMethodSchedule scheduled;
  while (!ready.empty()) {
    unsigned selected = ready.top().index;
    ready.pop();
    scheduled.expressions.push_back(original[selected]);
    for (unsigned input : graph.expressions[original[selected]].operands)
      if (auto found = producer.find(input); found != producer.end())
        --remainingUses[found->second];
    for (unsigned next : successors[selected])
      if (--indegree[next] == 0)
        enqueue(next);
  }
  if (scheduled.expressions.size() != n)
    return fixed;
  auto cutData = [&](llvm::ArrayRef<unsigned> order) {
    llvm::DenseMap<unsigned, unsigned> position;
    for (auto [i, id] : llvm::enumerate(order))
      position.try_emplace(graph.expressions[id].result, i);
    llvm::SmallVector<unsigned> last(n);
    for (unsigned i = 0; i < n; ++i)
      last[i] = i;
    for (auto [i, id] : llvm::enumerate(order))
      for (unsigned input : graph.expressions[id].operands)
        if (auto found = position.find(input); found != position.end())
          last[found->second] = std::max<unsigned>(last[found->second], i);
    return last;
  };
  auto last = cutData(scheduled.expressions);
  const unsigned parts = fixed.ends.size(), slack = parts * limit - n;
  // The donor's full deficit DP is quadratic in a large CLI chunk limit.
  // Keep exact states for small cuts and deterministic endpoint-preserving
  // sampling otherwise; bound work before allocating the predecessor table.
  constexpr uint64_t maxWork = 4000000;
  const uint64_t perStateWork = static_cast<uint64_t>(parts) *
                                (static_cast<uint64_t>(limit) + 65);
  unsigned states = static_cast<unsigned>(std::min<uint64_t>(
      std::min<uint64_t>(64, static_cast<uint64_t>(slack) + 1),
      maxWork / std::max<uint64_t>(1, perStateWork)));
  if (states == 0 || (slack && states < 2))
    return fixed;
  std::vector<unsigned> deficits;
  for (unsigned i = 0; i < states; ++i)
    deficits.push_back(states == 1 ? 0 :
        static_cast<uint64_t>(i) * slack / (states - 1));
  std::vector<uint64_t> previous(states, inf), current(states, inf);
  std::vector<std::vector<unsigned>> pred(parts + 1,
      std::vector<unsigned>(states, ~0u));
  previous[0] = 0;
  for (unsigned part = 1; part <= parts; ++part) {
    std::fill(current.begin(), current.end(), inf);
    for (unsigned state = 0; state < states; ++state) {
      unsigned deficit = deficits[state];
      unsigned end = part * limit - deficit;
      if (end == 0 || end > n)
        continue;
      unsigned maxLength = std::min(limit, end);
      std::vector<uint64_t> cost(maxLength + 1, 0);
      for (unsigned length = 1; length <= maxLength; ++length) {
        unsigned pos = end - length;
        unsigned value = graph.expressions[scheduled.expressions[pos]].result;
        cost[length] = cost[length - 1];
        if (last[pos] >= end)
          cost[length] = std::min(inf, cost[length] + weight[producer.lookup(value)]);
      }
      for (unsigned prior = 0; prior <= state; ++prior) {
        unsigned priorDeficit = deficits[prior];
        if (previous[prior] == inf || (part - 1) * limit < priorDeficit)
          continue;
        unsigned begin = (part - 1) * limit - priorDeficit, length = end - begin;
        if (length == 0 || length > maxLength)
          continue;
        uint64_t candidate = std::min(inf, previous[prior] + cost[length]);
        if (candidate < current[state]) {
          current[state] = candidate;
          pred[part][state] = prior;
        }
      }
    }
    previous.swap(current);
  }
  if (previous.back() == inf)
    return fixed;
  unsigned state = states - 1;
  for (unsigned part = parts; part > 0; --part) {
    scheduled.ends.push_back(part * limit - deficits[state]);
    state = pred[part][state];
  }
  std::reverse(scheduled.ends.begin(), scheduled.ends.end());
  auto score = [&](const PureMethodSchedule &schedule) {
    auto lastUse = cutData(schedule.expressions);
    uint64_t cost = 0;
    unsigned begin = 0, crosses = 0;
    for (unsigned end : schedule.ends) {
      for (unsigned i = begin; i < end; ++i)
        if (lastUse[i] >= end) {
          unsigned value = graph.expressions[schedule.expressions[i]].result;
          uint64_t w = weight[producer.lookup(value)];
          cost = std::min(inf, cost + w);
          crosses += w != 0;
        }
      begin = end;
    }
    return std::make_pair(cost, crosses);
  };
  return score(scheduled) <= score(fixed) ? scheduled : fixed;
}

static void planGroupStatements(SimulationPlan &plan) {
  for (unsigned nodeId : plan.graph.groupedNodes) {
    const SimNode &group = plan.graph.topNodes[nodeId];
    GroupStatementPlan statements;
    statements.replicatedExpressionIds = group.replicatedExpressionIds;
    PureMethodSchedule schedule;
    if (group.muxConditionBatches.empty())
      schedule = planPureMethods(plan.graph, group.expressionIds, plan.combChunkNodes);
    else {
      schedule.expressions = group.expressionIds;
      for (unsigned begin = 0; begin < group.expressionIds.size(); begin += plan.combChunkNodes)
        schedule.ends.push_back(std::min<unsigned>(group.expressionIds.size(), begin + plan.combChunkNodes));
    }
    unsigned begin = 0;
    for (unsigned end : schedule.ends) {
      llvm::SmallVector<SimStatement> chunk;
      for (unsigned index = begin; index < end;) {
        const MuxConditionBatch *batch = nullptr;
        for (const MuxConditionBatch &candidate : group.muxConditionBatches)
          if (candidate.begin == index && candidate.end <= end) {
            batch = &candidate;
            break;
          }
        SimStatement statement;
        if (!batch) {
          statement.expressionId = schedule.expressions[index++];
        } else {
          statement.kind = SimStatementKind::MuxCondition;
          statement.selectorId = batch->selectorId;
          for (unsigned i = batch->begin; i < batch->end; ++i) {
            const SimExpr &expr =
                plan.graph.expressions[group.expressionIds[i]];
            statement.muxAssignments.push_back(
                {expr.result, expr.operands[1], expr.operands[2]});
          }
          index = batch->end;
        }
        chunk.push_back(std::move(statement));
      }
      statements.chunks.push_back(std::move(chunk));
      begin = end;
    }
    plan.groupStatements.try_emplace(nodeId, std::move(statements));
  }
}

static void planCombRegionStatements(SimulationPlan &plan) {
  for (auto [regionId, region] : llvm::enumerate(plan.graph.combRegions)) {
    CombRegionStatementPlan statements;
    if (region.steps.empty())
      statements.chunks.emplace_back();
    bool pureExpressions = llvm::all_of(region.steps, [](const SimCombStep &step) {
      return step.expressionId != ~0u;
    });
    if (pureExpressions && !region.steps.empty()) {
      llvm::SmallVector<unsigned> ids;
      for (const SimCombStep &step : region.steps)
        ids.push_back(step.expressionId);
      auto schedule = planPureMethods(plan.graph, ids, plan.combChunkNodes);
      unsigned begin = 0;
      for (unsigned end : schedule.ends) {
        auto &chunk = statements.chunks.emplace_back();
        for (; begin < end; ++begin)
          chunk.push_back({schedule.expressions[begin], ~0u});
      }
    } else {
      for (unsigned begin = 0; begin < region.steps.size();) {
        unsigned end = begin + std::min<unsigned>(
                                   plan.combChunkNodes,
                                   region.steps.size() - begin);
        statements.chunks.emplace_back(region.steps.begin() + begin,
                                        region.steps.begin() + end);
        begin = end;
      }
    }
    plan.combRegionStatements.try_emplace(
        static_cast<unsigned>(regionId), std::move(statements));
  }
}

static SimNodeAction planNodeAction(const SimulationPlan &plan, unsigned id) {
  const SimNode &node = plan.graph.topNodes[id];
  SimNodeAction action;
  if (plan.groupByNode.count(id)) {
    action.kind = SimNodeActionKind::Group;
  } else if (node.kind == SimNodeKind::Assign) {
    action.kind = SimNodeActionKind::Assign;
    action.targetId = node.assignedValueId;
    action.sourceId = node.assignedFromId;
  } else if (node.kind == SimNodeKind::Assert) {
    action.kind = SimNodeActionKind::Assert;
    action.conditionId = node.assertionConditionId;
    action.message = node.assertionMessage;
  } else if (node.kind == SimNodeKind::CombRegion) {
    action.kind = SimNodeActionKind::CombRegion;
    action.regionId = node.combRegionId;
  } else if ((node.kind == SimNodeKind::Expression ||
              node.kind == SimNodeKind::PureGroup) &&
             node.expressionIds.size() == 1) {
    action.kind = SimNodeActionKind::Expression;
    action.expressionId = node.expressionIds.front();
  } else if (node.kind == SimNodeKind::Fifo) {
    action.kind = SimNodeActionKind::Fifo;
  } else if (node.kind == SimNodeKind::AsyncFifo) {
    action.kind = SimNodeActionKind::AsyncFifo;
  } else if (node.kind == SimNodeKind::ByteMem) {
    action.kind = SimNodeActionKind::ByteMem;
  } else if (node.kind == SimNodeKind::Instance) {
    action.kind = SimNodeActionKind::Instance;
  }
  return action;
}

static llvm::DenseMap<unsigned, unsigned>
planRegisterProbeTargets(const SimulationPlan &plan) {
  const SimGraph &graph = plan.graph;
  llvm::DenseSet<unsigned> registerOutputs;
  for (unsigned id : plan.operationOrder.regs) {
    if (id < graph.topNodes.size() &&
        graph.topNodes[id].outputIds.size() == 1)
      registerOutputs.insert(graph.topNodes[id].outputIds.front());
  }
  llvm::DenseMap<unsigned, unsigned> passthrough;
  for (const SimExpr &expr : graph.expressions)
    if (expr.kind == SimExprKind::Alias && expr.operands.size() == 1)
      passthrough.try_emplace(expr.result, expr.operands.front());
  for (const SimCombRegion &region : graph.combRegions) {
    for (auto [arg, input] : llvm::zip(region.argumentIds, region.inputIds))
      passthrough.try_emplace(arg, input);
    for (auto [result, yielded] : llvm::zip(region.resultIds, region.yieldIds))
      passthrough.try_emplace(result, yielded);
  }
  llvm::DenseSet<unsigned> roots(graph.outputValueIds.begin(),
                                 graph.outputValueIds.end());
  roots.insert(graph.namedProbeValueIds.begin(),
               graph.namedProbeValueIds.end());
  llvm::DenseMap<unsigned, unsigned> targets;
  for (unsigned root : roots) {
    llvm::DenseSet<unsigned> seen;
    unsigned value = root;
    while (seen.insert(value).second) {
      if (registerOutputs.contains(value)) {
        targets.try_emplace(root, value);
        break;
      }
      auto next = passthrough.find(value);
      if (next == passthrough.end())
        break;
      value = next->second;
    }
  }
  return targets;
}

static llvm::SmallVector<llvm::SmallVector<unsigned>>
planPrimitiveEvalChunks(const SimulationPlan &plan) {
  llvm::SmallVector<llvm::SmallVector<unsigned>> chunks;
  auto append = [&](const llvm::SmallVector<unsigned> &ids) {
    for (unsigned id : ids) {
      if (chunks.empty() || chunks.back().size() == plan.primitiveGroupSize)
        chunks.emplace_back();
      chunks.back().push_back(id);
    }
  };
  append(plan.operationOrder.instances);
  append(plan.operationOrder.fifos);
  append(plan.operationOrder.asyncFifos);
  append(plan.operationOrder.byteMems);
  return chunks;
}

static uint64_t cppPlacementWeight(const SimType &type) {
  uint64_t weight = 1 + (static_cast<uint64_t>(type.width) + 63) / 64;
  for (int64_t dimension : type.shape) {
    if (dimension <= 0 || static_cast<uint64_t>(dimension) >
                              std::numeric_limits<uint64_t>::max() / weight)
      return std::numeric_limits<uint64_t>::max() / 4;
    weight *= static_cast<uint64_t>(dimension);
  }
  return weight;
}

struct PlannedCppStorage {
  llvm::SmallVector<CppValueStorage> storage;
  std::vector<std::string> owners;
  std::map<std::string, llvm::SmallVector<unsigned>> locals;
  CppPlacementSummary summary;
};

// A method-local value must be defined on every execution before all of its
// reads in that same method. Persistent references are a separate, explicit
// root set; in particular caches and old-output snapshots are not SSA uses.
static PlannedCppStorage planCppStorage(const SimulationPlan &plan) {
  const SimGraph &graph = plan.graph;
  const unsigned count = graph.values.size();
  PlannedCppStorage result;
  result.storage.assign(count, CppValueStorage::Struct);
  result.owners.resize(count);
  std::vector<std::set<std::string>> reads(count), writes(count);
  llvm::DenseSet<unsigned> pinned, candidates;
  llvm::DenseMap<unsigned, unsigned> bindings;
  for (const SimCombRegion &region : graph.combRegions)
    for (auto [arg, input] : llvm::zip(region.argumentIds, region.inputIds))
      bindings.try_emplace(arg, input);
  auto canonical = [&](unsigned value) {
    // Nested comb block arguments are C++ aliases, not separate storage.
    for (unsigned depth = 0; depth < count; ++depth) {
      auto next = bindings.find(value);
      if (next == bindings.end())
        break;
      value = next->second;
    }
    return value;
  };
  auto pin = [&](unsigned value) { pinned.insert(canonical(value)); };
  auto pinAll = [&](const auto &values) {
    for (unsigned value : values)
      pin(value);
  };
  pinAll(graph.inputValueIds);
  pinAll(graph.outputValueIds);
  pinAll(graph.namedProbeValueIds);
  for (unsigned value = 0; value < count; ++value)
    if (graph.values[value].observable ||
        graph.values[value].sourceNameIsExplicit)
      pin(value);
  for (const SimEdge &edge : graph.edges)
    pin(edge.valueId);
  for (auto [id, node] : llvm::enumerate(graph.topNodes)) {
    if (plan.groupByNode.count(static_cast<unsigned>(id)))
      continue;
    // Includes ports, state/primitive reference bindings, ordinary eval
    // expressions, assigns and the external interface of explicit combs.
    pinAll(node.inputIds);
    pinAll(node.outputIds);
    if (node.assignedValueId != ~0u)
      pin(node.assignedValueId);
    if (node.assignedFromId != ~0u)
      pin(node.assignedFromId);
  }
  for (const auto &entry : plan.groupActivations)
    pinAll(entry.second.inputIds);
  for (const auto &entry : plan.groupPropagations)
    for (const GroupPropagationTarget &target : entry.second)
      pinAll(target.valueIds);
  pinAll(plan.fingerprintInputs);
  for (const auto &entry : plan.registerProbeTargets) {
    pin(entry.first);
    pin(entry.second);
  }

  using InlineExpressions = llvm::DenseMap<unsigned, unsigned>;
  std::function<void(unsigned, const std::string &, const InlineExpressions &)> read;
  read = [&](unsigned value, const std::string &owner,
             const InlineExpressions &inlined) {
    auto expression = inlined.find(value);
    if (expression != inlined.end()) {
      for (unsigned operand : graph.expressions[expression->second].operands)
        read(operand, owner, inlined);
      return;
    }
    reads[canonical(value)].insert(owner);
  };
  auto expression = [&](unsigned id, const std::string &owner,
                         InlineExpressions *inlined) {
    const SimExpr &expr = graph.expressions[id];
    candidates.insert(expr.result);
    if (expr.omitOriginalEvaluation)
      return;
    if (inlined && expr.inlineIntoConsumer) {
      inlined->try_emplace(expr.result, id);
      return;
    }
    writes[canonical(expr.result)].insert(owner);
    const InlineExpressions empty;
    for (unsigned operand : expr.operands)
      read(operand, owner, inlined ? *inlined : empty);
  };
  for (unsigned nodeId : graph.groupedNodes) {
    const auto &statements = plan.groupStatements.find(nodeId)->second;
    const std::string method = "eval_sim_group_" +
                              std::to_string(plan.groupByNode.lookup(nodeId));
    InlineExpressions inlined;
    for (unsigned id : statements.replicatedExpressionIds)
      expression(id, method, &inlined);
    for (auto [part, chunk] : llvm::enumerate(statements.chunks)) {
      const std::string owner = statements.chunks.size() > 1
                                   ? method + "_part_" + std::to_string(part)
                                   : method;
      for (const SimStatement &statement : chunk) {
        if (statement.kind == SimStatementKind::Expression) {
          expression(statement.expressionId, owner, &inlined);
        } else {
          read(statement.selectorId, owner, inlined);
          for (const PlannedMuxAssignment &assignment : statement.muxAssignments) {
            candidates.insert(assignment.resultId);
            writes[canonical(assignment.resultId)].insert(owner);
            read(assignment.trueValueId, owner, inlined);
            read(assignment.falseValueId, owner, inlined);
          }
        }
      }
    }
  }
  const InlineExpressions empty;
  std::function<void(unsigned, const std::string &)> nested;
  auto yields = [&](const SimCombRegion &region, const std::string &owner) {
    for (auto [value, yielded] : llvm::zip(region.resultIds, region.yieldIds)) {
      candidates.insert(value);
      writes[canonical(value)].insert(owner);
      read(yielded, owner, empty);
    }
  };
  nested = [&](unsigned id, const std::string &owner) {
    for (const auto &chunk : plan.combRegionStatements.find(id)->second.chunks)
      for (const SimCombStep &step : chunk) {
        if (step.expressionId != ~0u)
          expression(step.expressionId, owner, nullptr);
        else
          nested(step.nestedRegionId, owner);
      }
    yields(graph.combRegions[id], owner);
  };
  for (auto [index, nodeId] : llvm::enumerate(plan.operationOrder.combs)) {
    const unsigned regionId = graph.topNodes[nodeId].combRegionId;
    const auto &statements = plan.combRegionStatements.find(regionId)->second;
    const std::string method = "eval_comb_" + std::to_string(index);
    for (auto [part, chunk] : llvm::enumerate(statements.chunks)) {
      const std::string owner = statements.chunks.size() > 1
                                   ? method + "_part_" + std::to_string(part)
                                   : method;
      for (const SimCombStep &step : chunk) {
        if (step.expressionId != ~0u)
          expression(step.expressionId, owner, nullptr);
        else
          nested(step.nestedRegionId, owner);
      }
    }
    yields(graph.combRegions[regionId], method);
  }
  for (unsigned value : graph.declarationValueIds) {
    const SimValue &signal = graph.values[value];
    if (!pinned.contains(value) && candidates.contains(value)) {
      if (reads[value].empty() && writes[value].empty()) {
        result.storage[value] = CppValueStorage::Omitted;
        ++result.summary.omittedValues;
        continue;
      }
      if (writes[value].size() == 1 &&
          (reads[value].empty() || reads[value] == writes[value])) {
        result.storage[value] = CppValueStorage::Local;
        result.owners[value] = *writes[value].begin();
        result.locals[result.owners[value]].push_back(value);
        ++result.summary.localInMethod;
        continue;
      }
      if (!writes[value].empty()) {
        ++result.summary.crossPartPromoted;
        ++result.summary.scheduledCrossMethod;
        result.summary.scheduledCutWeight += cppPlacementWeight(signal.type);
      }
    }
    ++result.summary.structMembers;
    if (signal.observable || signal.sourceNameIsExplicit)
      ++result.summary.probePinnedStruct;
  }
  return result;
}

template <typename T>
static llvm::SmallVector<llvm::SmallVector<T>>
planTickChunks(llvm::ArrayRef<T> actions, unsigned limit) {
  llvm::SmallVector<llvm::SmallVector<T>> chunks;
  for (const T &action : actions) {
    if (chunks.empty() || chunks.back().size() == limit)
      chunks.emplace_back();
    chunks.back().push_back(action);
  }
  return chunks;
}

static void planLocalTickActions(
    const SimulationPlan &plan, llvm::SmallVector<SimTickAction> &compute,
    llvm::SmallVector<SimTickAction> &commit) {
  llvm::DenseSet<unsigned> groupedRegs;
  for (auto [index, group] : llvm::enumerate(plan.resetGroups)) {
    compute.push_back({SimTickActionKind::ResetGroup,
                       static_cast<unsigned>(index)});
    groupedRegs.insert(group.regNodeIds.begin(), group.regNodeIds.end());
  }
  for (unsigned id : plan.operationOrder.regs)
    if (!groupedRegs.contains(id))
      compute.push_back({SimTickActionKind::Reg, id});
  auto append = [&](SimTickActionKind kind,
                    const llvm::SmallVector<unsigned> &ids) {
    for (unsigned id : ids)
      compute.push_back({kind, id});
  };
  append(SimTickActionKind::Fifo, plan.operationOrder.fifos);
  append(SimTickActionKind::ByteMem, plan.operationOrder.byteMems);
  append(SimTickActionKind::SyncMem, plan.operationOrder.syncMems);
  append(SimTickActionKind::SyncMemDP, plan.operationOrder.syncMemDPs);
  append(SimTickActionKind::AsyncFifo, plan.operationOrder.asyncFifos);
  append(SimTickActionKind::CdcSync, plan.operationOrder.cdcSyncs);

  for (unsigned id : plan.operationOrder.regs)
    commit.push_back({SimTickActionKind::Reg, id});
  auto appendCommit = [&](SimTickActionKind kind,
                          const llvm::SmallVector<unsigned> &ids) {
    for (unsigned id : ids)
      commit.push_back({kind, id});
  };
  appendCommit(SimTickActionKind::Fifo, plan.operationOrder.fifos);
  appendCommit(SimTickActionKind::ByteMem, plan.operationOrder.byteMems);
  appendCommit(SimTickActionKind::SyncMem, plan.operationOrder.syncMems);
  appendCommit(SimTickActionKind::SyncMemDP, plan.operationOrder.syncMemDPs);
  appendCommit(SimTickActionKind::AsyncFifo, plan.operationOrder.asyncFifos);
  appendCommit(SimTickActionKind::CdcSync, plan.operationOrder.cdcSyncs);
}

} // namespace

LogicalResult SimulationPlan::verify() const {
  if (!graph.function || stepPhases.size() != 4 ||
      stepPhases[0] != SimulationPhase::Comb ||
      stepPhases[1] != SimulationPhase::TickCompute ||
      stepPhases[2] != SimulationPhase::TickCommit ||
      stepPhases[3] != SimulationPhase::Comb ||
      evalChunkNodes == 0 || combChunkNodes == 0 ||
      sccChunkNodes == 0 || tickChunkNodes == 0 || primitiveGroupSize == 0)
    return failure();
  if (fallbackIterationLimit != primitiveCount)
    return failure();
  if (nodeActions.size() != graph.topNodes.size())
    return failure();
  for (unsigned id = 0; id < nodeActions.size(); ++id) {
    const SimNodeAction &action = nodeActions[id];
    SimNodeAction expected = planNodeAction(*this, id);
    if (action.kind != expected.kind ||
        action.targetId != expected.targetId ||
        action.sourceId != expected.sourceId ||
        action.conditionId != expected.conditionId ||
        action.message != expected.message ||
        action.regionId != expected.regionId ||
        action.expressionId != expected.expressionId)
      return failure();
    const SimNode &node = graph.topNodes[id];
    if (action.kind == SimNodeActionKind::Skip && includeForEval(node, true))
      return failure();
  }
  auto expectedProbeTargets = planRegisterProbeTargets(*this);
  if (registerProbeTargets.size() != expectedProbeTargets.size())
    return failure();
  for (const auto &entry : expectedProbeTargets)
    if (auto it = registerProbeTargets.find(entry.first);
        it == registerProbeTargets.end() || it->second != entry.second)
      return failure();
  if (groupByNode.size() != graph.groupedNodes.size())
    return failure();
  if (combRegionStatements.size() != graph.combRegions.size())
    return failure();
  for (auto [regionId, region] : llvm::enumerate(graph.combRegions)) {
    auto found = combRegionStatements.find(static_cast<unsigned>(regionId));
    if (found == combRegionStatements.end())
      return failure();
    const auto &chunks = found->second.chunks;
    llvm::SmallVector<SimCombStep> expectedSteps = region.steps;
    llvm::SmallVector<unsigned> ends;
    bool pureExpressions = llvm::all_of(region.steps, [](const SimCombStep &step) {
      return step.expressionId != ~0u;
    });
    if (pureExpressions && !region.steps.empty()) {
      llvm::SmallVector<unsigned> ids;
      for (const SimCombStep &step : region.steps)
        ids.push_back(step.expressionId);
      auto schedule = planPureMethods(graph, ids, combChunkNodes);
      ends = std::move(schedule.ends);
      for (auto [i, expression] : llvm::enumerate(schedule.expressions))
        expectedSteps[i].expressionId = expression;
    } else {
      if (region.steps.empty())
        ends.push_back(0);
      for (unsigned begin = 0; begin < region.steps.size(); begin += combChunkNodes)
        ends.push_back(std::min<unsigned>(region.steps.size(), begin + combChunkNodes));
    }
    if (chunks.size() != ends.size())
      return failure();
    unsigned cursor = 0;
    for (auto [part, chunk] : llvm::enumerate(chunks)) {
      unsigned expectedSize = ends[part] - cursor;
      if (chunk.size() != expectedSize || chunk.size() > combChunkNodes)
        return failure();
      for (const SimCombStep &step : chunk) {
        const SimCombStep &expected = expectedSteps[cursor++];
        if (step.expressionId != expected.expressionId ||
            step.nestedRegionId != expected.nestedRegionId)
          return failure();
      }
    }
    if (cursor != region.steps.size())
      return failure();
  }
  unsigned expectedActivations = 0;
  for (auto [i, nodeIndex] : llvm::enumerate(graph.groupedNodes)) {
    if (nodeIndex >= graph.topNodes.size())
      return failure();
    const SimNode &node = graph.topNodes[nodeIndex];
    if (node.expressionIds.empty() || !groupByNode.count(nodeIndex) ||
        groupByNode.lookup(nodeIndex) != i)
      return failure();
    if (node.activateOnInputChange) {
      ++expectedActivations;
      auto it = groupActivations.find(nodeIndex);
      if (it == groupActivations.end() || it->second.inputIds != node.inputIds ||
          it->second.usedBits != node.activationUsedBits ||
          it->second.vectorLanes != node.activationVectorLanes ||
          it->second.vectorElements != node.activationVectorElements)
        return failure();
    } else if (groupActivations.count(nodeIndex)) {
      return failure();
    }
  }
  if (groupActivations.size() != expectedActivations)
    return failure();
  if (groupStatements.size() != graph.groupedNodes.size())
    return failure();
  for (unsigned nodeId : graph.groupedNodes) {
    const SimNode &group = graph.topNodes[nodeId];
    auto found = groupStatements.find(nodeId);
    if (found == groupStatements.end() ||
        found->second.replicatedExpressionIds !=
            group.replicatedExpressionIds)
      return failure();
    const GroupStatementPlan &statements = found->second;
    PureMethodSchedule schedule;
    if (group.muxConditionBatches.empty())
      schedule = planPureMethods(graph, group.expressionIds, combChunkNodes);
    else {
      schedule.expressions = group.expressionIds;
      for (unsigned begin = 0; begin < group.expressionIds.size(); begin += combChunkNodes)
        schedule.ends.push_back(std::min<unsigned>(group.expressionIds.size(), begin + combChunkNodes));
    }
    if (statements.chunks.size() != schedule.ends.size())
      return failure();
    unsigned cursor = 0;
    for (auto [part, chunk] : llvm::enumerate(statements.chunks)) {
      unsigned chunkEnd = schedule.ends[part];
      if (chunkEnd <= cursor || chunkEnd - cursor > combChunkNodes)
        return failure();
      for (const SimStatement &statement : chunk) {
        if (cursor >= chunkEnd)
          return failure();
        if (statement.kind == SimStatementKind::Expression) {
          if (statement.expressionId != schedule.expressions[cursor] ||
              statement.selectorId != ~0u ||
              !statement.muxAssignments.empty())
            return failure();
          ++cursor;
          continue;
        }
        unsigned count = statement.muxAssignments.size();
        if (statement.kind != SimStatementKind::MuxCondition ||
            statement.expressionId != ~0u || count <= 5 ||
            count > chunkEnd - cursor)
          return failure();
        bool annotated = false;
        for (const MuxConditionBatch &batch : group.muxConditionBatches)
          if (batch.begin == cursor && batch.end == cursor + count &&
              batch.selectorId == statement.selectorId)
            annotated = true;
        if (!annotated)
          return failure();
        for (unsigned offset = 0; offset < count; ++offset) {
          const SimExpr &expr =
              graph.expressions[group.expressionIds[cursor + offset]];
          const PlannedMuxAssignment &assignment =
              statement.muxAssignments[offset];
          if ((expr.kind != SimExprKind::Mux && expr.kind != SimExprKind::Select) ||
              expr.operands.size() != 3 ||
              expr.operands[0] != statement.selectorId ||
              assignment.resultId != expr.result ||
              assignment.trueValueId != expr.operands[1] ||
              assignment.falseValueId != expr.operands[2])
            return failure();
        }
        cursor += count;
      }
      if (cursor != chunkEnd)
        return failure();
    }
  }
  if ((!combTopological || graph.hasAmbiguousDrivers) &&
      (!packedActivationGroups.empty() || !groupPropagations.empty()))
    return failure();
  llvm::DenseMap<unsigned, unsigned> combPosition;
  for (auto [position, id] : llvm::enumerate(combNodeOrder))
    combPosition.try_emplace(id, static_cast<unsigned>(position));
  llvm::DenseMap<unsigned, unsigned> evalPosition;
  for (auto [position, id] : llvm::enumerate(evalNodeOrder))
    evalPosition.try_emplace(id, static_cast<unsigned>(position));
  for (unsigned target : packedActivationGroups) {
    if (!groupByNode.count(target) || !groupActivations.count(target))
      return failure();
    for (unsigned input : graph.topNodes[target].inputIds) {
      bool covered = false;
      for (const SimEdge &edge : graph.edges)
        if (edge.consumer == target && edge.valueId == input &&
            canPublishActivity(*this, edge.producer, target, combPosition,
                               evalPosition))
          covered = true;
      if (!covered)
        return failure();
    }
  }
  std::set<std::tuple<unsigned, unsigned, unsigned>> expectedPropagation;
  for (const SimEdge &edge : graph.edges)
    if (packedActivationGroups.contains(edge.consumer))
      expectedPropagation.emplace(edge.producer, edge.consumer, edge.valueId);
  std::set<std::tuple<unsigned, unsigned, unsigned>> actualPropagation;
  llvm::DenseSet<unsigned> expectedCommitSources;
  for (const auto &entry : groupPropagations) {
    if (entry.first >= graph.topNodes.size())
      return failure();
    if (publishesOnCommit(graph.topNodes[entry.first]))
      expectedCommitSources.insert(entry.first);
    for (const GroupPropagationTarget &target : entry.second) {
      if (!packedActivationGroups.contains(target.groupNodeId) ||
          !canPublishActivity(*this, entry.first, target.groupNodeId,
                              combPosition, evalPosition) ||
          target.valueIds.empty() ||
          target.usedBits.size() != target.valueIds.size() ||
          target.vectorLanes.size() != target.valueIds.size() ||
          target.vectorElements.size() != target.valueIds.size())
        return failure();
      const GroupActivationPlan &activation =
          groupActivations.find(target.groupNodeId)->second;
      for (auto [i, value] : llvm::enumerate(target.valueIds)) {
        auto input = llvm::find(activation.inputIds, value);
        if (input == activation.inputIds.end() || value >= graph.values.size() ||
            !graph.values[value].sourceBacked)
          return failure();
        unsigned index = static_cast<unsigned>(input - activation.inputIds.begin());
        const SimType &type = graph.values[value].type;
        llvm::APInt expected = index < activation.usedBits.size()
                                   ? activation.usedBits[index]
                                   : llvm::APInt(type.shape.empty() && type.width
                                                     ? type.width : 1,
                                                 0, true);
        if (target.usedBits[i] != expected)
          return failure();
        llvm::APInt expectedLanes =
            index < activation.vectorLanes.size()
                ? activation.vectorLanes[index]
                : llvm::APInt::getAllOnes(
                      type.shape.empty()
                          ? 1u
                          : static_cast<unsigned>(type.shape.front()));
        if (target.vectorLanes[i] != expectedLanes)
          return failure();
        llvm::APInt expectedElements =
            index < activation.vectorElements.size()
                ? activation.vectorElements[index]
                : llvm::APInt(1, 1);
        if (target.vectorElements[i] != expectedElements)
          return failure();
        if (!actualPropagation.emplace(entry.first, target.groupNodeId,
                                       value).second)
          return failure();
      }
    }
  }
  if (expectedPropagation != actualPropagation)
    return failure();
  if (expectedCommitSources != commitPropagationNodes)
    return failure();
  if (activityPublications.size() != groupPropagations.size())
    return failure();
  for (const auto &[source, targets] : groupPropagations) {
    auto found = activityPublications.find(source);
    if (found == activityPublications.end())
      return failure();
    llvm::DenseSet<unsigned> covered;
    for (const ActivityPublication &pub : found->second) {
      if (pub.stores.empty() || pub.branchless != (pub.stores.size() <= 3))
        return failure();
      std::map<unsigned, uint64_t> expected;
      for (const GroupPropagationTarget &target : targets)
        if (sameActivityPredicate(pub.predicate, target)) {
          if (!covered.insert(target.groupNodeId).second)
            return failure();
          unsigned group = groupByNode.lookup(target.groupNodeId);
          expected[group / 64u] |= uint64_t{1} << (group % 64u);
        }
      if (pub.stores.size() != expected.size())
        return failure();
      for (const ActivityWordMask &store : pub.stores) {
        auto mask = expected.find(store.word);
        if (mask == expected.end() || store.mask != mask->second)
          return failure();
        expected.erase(mask);
      }
    }
    if (covered.size() != targets.size())
      return failure();
  }
  auto validBatches = [&](llvm::ArrayRef<ActivityBatch> batches,
                          llvm::ArrayRef<unsigned> expected) {
    unsigned cursor = 0;
    for (const ActivityBatch &batch : batches) {
      if (batch.nodeIds.empty() || (!batch.mask && batch.nodeIds.size() != 1))
        return false;
      uint64_t mask = 0;
      unsigned byte = ~0u;
      for (unsigned node : batch.nodeIds) {
        if (cursor >= expected.size() || node != expected[cursor++])
          return false;
        if (batch.mask) {
          if (!packedActivationGroups.contains(node))
            return false;
          unsigned group = groupByNode.lookup(node);
          if (group / 64u != batch.word || (byte != ~0u && group / 8u != byte))
            return false;
          byte = group / 8u;
          mask |= uint64_t{1} << (group % 64u);
        } else if (packedActivationGroups.contains(node)) {
          return false;
        }
      }
      if (mask != batch.mask)
        return false;
    }
    return cursor == expected.size();
  };
  if (!validBatches(combActivityBatches, combNodeOrder))
    return failure();
  unsigned evalCursor = 0;
  for (const auto &chunk : evalActivityChunks) {
    if (chunk.empty() || evalCursor >= evalNodeOrder.size())
      return failure();
    unsigned size = std::min<unsigned>(evalChunkNodes, evalNodeOrder.size() - evalCursor);
    if (!validBatches(chunk, llvm::ArrayRef(evalNodeOrder).slice(evalCursor, size)))
      return failure();
    evalCursor += size;
  }
  if (evalCursor != evalNodeOrder.size())
    return failure();
  llvm::DenseSet<unsigned> batchedRegs;
  for (const ResetGroupPlan &group : resetGroups) {
    if (group.regNodeIds.size() < 2 ||
        group.clockValueId >= graph.values.size() ||
        group.resetValueId >= graph.values.size())
      return failure();
    llvm::SmallVector<unsigned> chunkRegs;
    for (const auto &chunk : group.regChunks) {
      if (chunk.empty() || chunk.size() > tickChunkNodes)
        return failure();
      chunkRegs.append(chunk.begin(), chunk.end());
    }
    if (chunkRegs != group.regNodeIds)
      return failure();
    for (unsigned id : group.regNodeIds) {
      if (id >= graph.topNodes.size() || !batchedRegs.insert(id).second)
        return failure();
      const SimNode &reg = graph.topNodes[id];
      if (reg.kind != SimNodeKind::Reg || reg.inputIds.size() != 5 ||
          reg.outputIds.size() != 1 ||
          reg.inputIds[0] != group.clockValueId ||
          reg.inputIds[1] != group.resetValueId)
        return failure();
    }
  }
  for (unsigned id : fingerprintInputs)
    if (id >= graph.values.size() ||
        !supportsScalarFingerprint(graph.values[id].type))
      return graph.function->emitError(
          "simulation plan input fingerprint requires a scalar of at most 64 bits");
  for (const auto &entry : instanceCaches) {
    if (entry.first >= graph.topNodes.size() ||
        graph.topNodes[entry.first].kind != SimNodeKind::Instance)
      return failure();
    const SimNode &instance = graph.topNodes[entry.first];
    auto expectedWords = instanceCacheWordCount(graph, instance);
    if (failed(expectedWords) || entry.second.packedWords != *expectedWords ||
        entry.second.usePackedWords !=
            (instance.inputIds.size() >= 12 || *expectedWords >= 16))
      return instance.op->emitError(
          "simulation instance input cache does not match its port shapes");
  }
  for (const auto &entry : instanceInterfaces)
    if (entry.first >= graph.topNodes.size() ||
        graph.topNodes[entry.first].kind != SimNodeKind::Instance ||
        entry.second.inputPorts.size() != graph.topNodes[entry.first].inputIds.size() ||
        entry.second.outputPorts.size() != graph.topNodes[entry.first].outputIds.size())
      return failure();
  for (unsigned id : invalidateCachesOnCommit)
    if (id >= graph.topNodes.size() ||
        (graph.topNodes[id].kind != SimNodeKind::Instance &&
         graph.topNodes[id].kind != SimNodeKind::Fifo &&
         graph.topNodes[id].kind != SimNodeKind::AsyncFifo &&
         graph.topNodes[id].kind != SimNodeKind::ByteMem))
      return failure();
  auto validOrder = [&](const llvm::SmallVector<unsigned> &ids) {
    llvm::DenseSet<unsigned> seen;
    for (unsigned id : ids)
      if (id >= graph.topNodes.size() || !seen.insert(id).second)
        return false;
    return true;
  };
  if (!validOrder(combNodeOrder) || !validOrder(evalNodeOrder))
    return failure();
  auto validTypedOrder = [&](const auto &ids, SimNodeKind kind) {
    llvm::DenseSet<unsigned> seen;
    for (unsigned id : ids)
      if (id >= graph.topNodes.size() || !seen.insert(id).second ||
          graph.topNodes[id].kind != kind)
        return false;
    unsigned expected = 0;
    for (const SimNode &node : graph.topNodes)
      expected += node.kind == kind;
    return ids.size() == expected;
  };
  if (!validTypedOrder(operationOrder.regs, SimNodeKind::Reg) ||
      !validTypedOrder(operationOrder.fifos, SimNodeKind::Fifo) ||
      !validTypedOrder(operationOrder.byteMems, SimNodeKind::ByteMem) ||
      !validTypedOrder(operationOrder.syncMems, SimNodeKind::SyncMem) ||
      !validTypedOrder(operationOrder.syncMemDPs, SimNodeKind::SyncMemDP) ||
      !validTypedOrder(operationOrder.asyncFifos, SimNodeKind::AsyncFifo) ||
      !validTypedOrder(operationOrder.cdcSyncs, SimNodeKind::CdcSync) ||
      !validTypedOrder(operationOrder.instances, SimNodeKind::Instance) ||
      !validTypedOrder(operationOrder.combs, SimNodeKind::CombRegion))
    return failure();
  llvm::SmallVector<unsigned> plannedInstances;
  for (const auto &chunk : instanceTickChunks) {
    if (chunk.empty() || chunk.size() > tickChunkNodes)
      return failure();
    plannedInstances.append(chunk.begin(), chunk.end());
  }
  if (plannedInstances != operationOrder.instances)
    return failure();
  if (primitiveEvalChunks != planPrimitiveEvalChunks(*this))
    return failure();
  llvm::SmallVector<SimTickAction> expectedCompute, expectedCommit;
  planLocalTickActions(*this, expectedCompute, expectedCommit);
  auto sameActions = [](const auto &actual, const auto &expected) {
    if (actual.size() != expected.size())
      return false;
    for (auto [a, b] : llvm::zip(actual, expected))
      if (a.kind != b.kind || a.id != b.id)
        return false;
    return true;
  };
  if (!sameActions(localTickComputeActions, expectedCompute) ||
      !sameActions(localTickCommitActions, expectedCommit))
    return failure();
  auto validTickChunks = [&](const auto &chunks, const auto &actions) {
    llvm::SmallVector<SimTickAction> flat;
    for (const auto &chunk : chunks) {
      if (chunk.empty() || chunk.size() > tickChunkNodes)
        return false;
      flat.append(chunk.begin(), chunk.end());
    }
    return sameActions(flat, actions);
  };
  if (!validTickChunks(localTickComputeChunks, localTickComputeActions) ||
      !validTickChunks(localTickCommitChunks, localTickCommitActions))
    return failure();
  for (const ScheduledComponent &component : sccOrder)
    if (!validOrder(component.nodeIds))
      return failure();
  for (auto [id, node] : llvm::enumerate(graph.topNodes)) {
    unsigned nodeId = static_cast<unsigned>(id);
    if (node.kind == SimNodeKind::Instance) {
      if (!instanceCaches.count(nodeId) || !instanceInterfaces.count(nodeId) ||
          instanceCaches.lookup(nodeId).invalidateOnCommit !=
              invalidateCachesOnCommit.contains(nodeId))
        return failure();
    }
    if ((node.kind == SimNodeKind::Fifo ||
         node.kind == SimNodeKind::AsyncFifo ||
         node.kind == SimNodeKind::ByteMem) &&
        !invalidateCachesOnCommit.contains(nodeId))
      return failure();
  }
  if (evalTopological) {
    unsigned expected = 0;
    for (const SimNode &node : graph.topNodes)
      expected += includeForEval(node, true);
    if (evalNodeOrder.size() != expected)
      return failure();
  }
  auto placement = planCppStorage(*this);
  const auto &a = cppPlacementSummary;
  const auto &b = placement.summary;
  if (cppValueStorage != placement.storage ||
      cppValueOwners != placement.owners || cppMethodLocals != placement.locals ||
      std::tie(a.structMembers, a.localInMethod, a.probePinnedStruct,
               a.crossPartPromoted, a.scheduledCrossMethod,
               a.scheduledCutWeight, a.omittedValues) !=
      std::tie(b.structMembers, b.localInMethod, b.probePinnedStruct,
               b.crossPartPromoted, b.scheduledCrossMethod,
               b.scheduledCutWeight, b.omittedValues))
    return graph.function->emitError("simulation C++ value placement is incomplete");
  return success();
}

FailureOr<SimulationPlan> buildSimulationPlan(SimGraph graph,
                                              const SimulationPlanningOptions &options) {
  if (failed(graph.verify()))
    return failure();
  func::FuncOp function = graph.function;
  SimulationPlan plan;
  plan.graph = std::move(graph);
  plan.stepPhases = {SimulationPhase::Comb, SimulationPhase::TickCompute,
                     SimulationPhase::TickCommit, SimulationPhase::Comb};
  for (auto [i, nodeIndex] : llvm::enumerate(plan.graph.groupedNodes))
    plan.groupByNode.try_emplace(nodeIndex, i);
  for (unsigned id = 0; id < plan.graph.topNodes.size(); ++id)
    plan.nodeActions.push_back(planNodeAction(plan, id));
  for (unsigned nodeIndex : plan.graph.groupedNodes) {
    const SimNode &node = plan.graph.topNodes[nodeIndex];
    if (node.activateOnInputChange)
      plan.groupActivations.try_emplace(
          nodeIndex, GroupActivationPlan{node.inputIds, node.activationUsedBits,
                                         node.activationVectorLanes,
                                         node.activationVectorElements});
  }
  plan.evalChunkNodes = std::max(1u, options.evalChunkNodes);
  plan.combChunkNodes = std::max(1u, options.combChunkNodes);
  planGroupStatements(plan);
  planCombRegionStatements(plan);

  for (auto [id, node] : llvm::enumerate(plan.graph.topNodes)) {
    unsigned nodeId = static_cast<unsigned>(id);
    if (node.kind == SimNodeKind::Fifo ||
        node.kind == SimNodeKind::AsyncFifo ||
        node.kind == SimNodeKind::ByteMem ||
        node.kind == SimNodeKind::Instance)
      ++plan.primitiveCount;
    if (node.kind == SimNodeKind::Instance) {
      InstanceCachePlan cache;
      auto packedWords = instanceCacheWordCount(plan.graph, node);
      if (failed(packedWords)) {
        node.op->emitError("simulation instance input cache exceeds supported word count");
        return failure();
      }
      cache.packedWords = *packedWords;
      for (unsigned inputId : node.inputIds) {
        if (supportsScalarFingerprint(plan.graph.values[inputId].type))
          plan.fingerprintInputs.insert(inputId);
      }
      cache.usePackedWords = node.inputIds.size() >= 12 ||
                             cache.packedWords >= 16;
      if (node.instanceCallee.empty() || !node.instanceCalleeResolved) {
        node.op->emitError("simulation plan: missing instance callee");
        return failure();
      }
      InstanceInterfacePlan interface = planInstanceInterface(node);
      if (interface.inputPorts.size() != node.inputIds.size() ||
          interface.outputPorts.size() != node.outputIds.size()) {
        node.op->emitError("simulation plan: instance ports do not match callee signature");
        return failure();
      }
      plan.instanceInterfaces.try_emplace(nodeId, std::move(interface));
      cache.invalidateOnCommit = node.stateBoundary;
      plan.instanceCaches.try_emplace(nodeId, cache);
      if (cache.invalidateOnCommit)
        plan.invalidateCachesOnCommit.insert(nodeId);
    }
    if (node.kind == SimNodeKind::Fifo) {
      unsigned dataId = node.inputIds[3];
      if (supportsScalarFingerprint(plan.graph.values[dataId].type))
        plan.fingerprintInputs.insert(dataId);
      plan.invalidateCachesOnCommit.insert(nodeId);
    }
    if (node.kind == SimNodeKind::AsyncFifo) {
      unsigned dataId = node.inputIds[5];
      if (supportsScalarFingerprint(plan.graph.values[dataId].type))
        plan.fingerprintInputs.insert(dataId);
      plan.invalidateCachesOnCommit.insert(nodeId);
    }
    if (node.kind == SimNodeKind::ByteMem) {
      for (unsigned index : {2u, 4u, 5u}) {
        unsigned valueId = node.inputIds[index];
        if (supportsScalarFingerprint(plan.graph.values[valueId].type))
          plan.fingerprintInputs.insert(valueId);
      }
      plan.invalidateCachesOnCommit.insert(nodeId);
    }
  }
  plan.fallbackIterationLimit = plan.primitiveCount;

  NodeKeys names(plan.graph);
  collectOperationOrder(plan.graph, names, plan.operationOrder);
  plan.primitiveEvalChunks = planPrimitiveEvalChunks(plan);
  for (unsigned begin = 0; begin < plan.operationOrder.instances.size();
       begin += plan.tickChunkNodes) {
    auto &chunk = plan.instanceTickChunks.emplace_back();
    unsigned end = std::min<unsigned>(plan.operationOrder.instances.size(),
                                      begin + plan.tickChunkNodes);
    chunk.append(plan.operationOrder.instances.begin() + begin,
                 plan.operationOrder.instances.begin() + end);
  }
  plan.registerProbeTargets = planRegisterProbeTargets(plan);
  std::map<std::pair<unsigned, unsigned>, llvm::SmallVector<unsigned>> resetGroups;
  for (unsigned id : plan.operationOrder.regs) {
    const SimNode &reg = plan.graph.topNodes[id];
    if (reg.inputIds.size() != 5 || reg.outputIds.size() != 1) {
      reg.op->emitError("simulation register graph mapping is incomplete");
      return failure();
    }
    resetGroups[{reg.inputIds[0], reg.inputIds[1]}].push_back(id);
  }
  for (auto &[key, regs] : resetGroups)
    if (regs.size() >= 2) {
      auto chunks = planTickChunks<unsigned>(regs, plan.tickChunkNodes);
      plan.resetGroups.push_back(ResetGroupPlan{key.first, key.second,
                                                std::move(regs),
                                                std::move(chunks)});
    }
  planLocalTickActions(plan, plan.localTickComputeActions,
                       plan.localTickCommitActions);
  plan.localTickComputeChunks = planTickChunks<SimTickAction>(
      plan.localTickComputeActions, plan.tickChunkNodes);
  plan.localTickCommitChunks = planTickChunks<SimTickAction>(
      plan.localTickCommitActions, plan.tickChunkNodes);
  DependencyGraph comb;
  if (buildDependencies(plan.graph, names, false, comb))
    plan.combTopological = topoOrder(comb, plan.combNodeOrder);
  if (!plan.combTopological) {
    for (auto [id, node] : llvm::enumerate(plan.graph.topNodes))
      if (node.kind != SimNodeKind::Return && node.kind != SimNodeKind::Wire) {
        plan.combNodeOrder.push_back(static_cast<unsigned>(id));
      }
  }
  DependencyGraph full;
  if (buildDependencies(plan.graph, names, true, full)) {
    plan.evalTopological = topoOrder(full, plan.evalNodeOrder);
    if (!plan.evalTopological)
      buildSccOrder(full, plan);
  }
  planPackedGroupActivation(plan);
  planActivityPublications(plan);
  plan.combActivityBatches = planActivityBatches(plan, plan.combNodeOrder);
  for (unsigned begin = 0; begin < plan.evalNodeOrder.size(); begin += plan.evalChunkNodes) {
    unsigned size = std::min<unsigned>(plan.evalChunkNodes, plan.evalNodeOrder.size() - begin);
    plan.evalActivityChunks.push_back(planActivityBatches(
        plan, llvm::ArrayRef(plan.evalNodeOrder).slice(begin, size)));
  }
  auto placement = planCppStorage(plan);
  plan.cppValueStorage = std::move(placement.storage);
  plan.cppValueOwners = std::move(placement.owners);
  plan.cppMethodLocals = std::move(placement.locals);
  plan.cppPlacementSummary = placement.summary;
  if (failed(plan.verify())) {
    function.emitError("simulation plan is incomplete");
    return failure();
  }
  return plan;
}

FailureOr<ModuleSimulationPlan> buildModuleSimulationPlan(
    ModuleOp module, std::vector<SimulationPlan> functions) {
  ModuleSimulationPlan result;
  result.module = module;
  auto moduleFuncs = module.getOps<func::FuncOp>();
  unsigned moduleFunctionCount =
      static_cast<unsigned>(std::distance(moduleFuncs.begin(), moduleFuncs.end()));
  if (functions.size() != moduleFunctionCount) {
    module.emitError("simulation plan: function count does not match source module");
    return failure();
  }
  llvm::SmallVector<std::string> names;
  for (const SimulationPlan &plan : functions) {
    if (!plan.graph.function || plan.graph.function->getParentOfType<ModuleOp>() != module ||
        failed(plan.verify()))
      return failure();
    names.push_back(plan.graph.functionName);
  }
  llvm::StringMap<unsigned> indexByName;
  for (auto [i, name] : llvm::enumerate(names))
    if (!indexByName.try_emplace(name, static_cast<unsigned>(i)).second) {
      module.emitError("simulation plan: duplicate function symbol");
      return failure();
    }
  llvm::SmallVector<llvm::SmallVector<unsigned>> succ(names.size());
  llvm::SmallVector<unsigned> indeg(names.size(), 0);
  for (auto [i, plan] : llvm::enumerate(functions)) {
    for (const SimNode &node : plan.graph.topNodes) {
      if (node.kind != SimNodeKind::Instance)
        continue;
      auto it = indexByName.find(node.instanceCallee);
      if (it == indexByName.end())
        continue;
      succ[it->second].push_back(static_cast<unsigned>(i));
      ++indeg[i];
    }
  }
  auto cmp = [&](unsigned a, unsigned b) { return names[a] > names[b]; };
  std::vector<unsigned> heap;
  for (unsigned i = 0; i < names.size(); ++i)
    if (indeg[i] == 0)
      heap.push_back(i);
  std::make_heap(heap.begin(), heap.end(), cmp);
  while (!heap.empty()) {
    std::pop_heap(heap.begin(), heap.end(), cmp);
    unsigned i = heap.back();
    heap.pop_back();
    result.functions.push_back(std::move(functions[i]));
    for (unsigned s : succ[i])
      if (--indeg[s] == 0) {
        heap.push_back(s);
        std::push_heap(heap.begin(), heap.end(), cmp);
      }
  }
  if (result.functions.size() != names.size()) {
    module.emitError("simulation plan: module instance graph has a cycle");
    return failure();
  }
  return result;
}

} // namespace pyc
