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
#include <map>
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

static void planPackedGroupActivation(SimulationPlan &plan) {
  if (!plan.combTopological || plan.graph.hasAmbiguousDrivers)
    return;
  llvm::DenseMap<unsigned, unsigned> position;
  for (auto [order, id] : llvm::enumerate(plan.combNodeOrder))
    position.try_emplace(id, static_cast<unsigned>(order));
  llvm::DenseMap<unsigned, llvm::DenseMap<unsigned, unsigned>> inputProducer;
  for (const SimEdge &edge : plan.graph.edges)
    if (plan.groupByNode.count(edge.producer) &&
        plan.groupActivations.count(edge.consumer))
      inputProducer[edge.consumer].try_emplace(edge.valueId, edge.producer);
  for (unsigned target : plan.graph.groupedNodes) {
    if (!plan.groupActivations.count(target))
      continue;
    bool allFromGroups = true;
    for (unsigned value : plan.graph.topNodes[target].inputIds) {
      auto it = inputProducer[target].find(value);
      if (it == inputProducer[target].end() ||
          !position.count(it->second) || !position.count(target) ||
          position.lookup(it->second) >= position.lookup(target)) {
        allFromGroups = false;
        break;
      }
    }
    if (allFromGroups)
      plan.packedActivationGroups.insert(target);
  }
  std::map<std::pair<unsigned, unsigned>, std::set<unsigned>> propagation;
  for (const SimEdge &edge : plan.graph.edges)
    if (plan.groupByNode.count(edge.producer) &&
        plan.packedActivationGroups.contains(edge.consumer))
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
  }
}

static void planGroupStatements(SimulationPlan &plan) {
  for (unsigned nodeId : plan.graph.groupedNodes) {
    const SimNode &group = plan.graph.topNodes[nodeId];
    GroupStatementPlan statements;
    statements.replicatedExpressionIds = group.replicatedExpressionIds;
    for (unsigned begin = 0; begin < group.expressionIds.size();) {
      unsigned end = begin + std::min<unsigned>(
                                 plan.combChunkNodes,
                                 group.expressionIds.size() - begin);
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
          statement.expressionId = group.expressionIds[index++];
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
    for (unsigned begin = 0; begin < region.steps.size();) {
      unsigned end = begin + std::min<unsigned>(
                                 plan.combChunkNodes,
                                 region.steps.size() - begin);
      statements.chunks.emplace_back(region.steps.begin() + begin,
                                      region.steps.begin() + end);
      begin = end;
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
    unsigned expectedChunks = region.steps.empty()
                                  ? 1
                                  : 1 + (region.steps.size() - 1) / combChunkNodes;
    if (chunks.size() != expectedChunks)
      return failure();
    unsigned cursor = 0;
    for (const auto &chunk : chunks) {
      unsigned expectedSize = std::min<unsigned>(
          combChunkNodes, region.steps.size() - cursor);
      if (chunk.size() != expectedSize)
        return failure();
      for (const SimCombStep &step : chunk) {
        const SimCombStep &expected = region.steps[cursor++];
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
    unsigned expectedChunks =
        1 + (group.expressionIds.size() - 1) / combChunkNodes;
    if (statements.chunks.size() != expectedChunks)
      return failure();
    unsigned cursor = 0;
    for (const auto &chunk : statements.chunks) {
      unsigned chunkEnd = cursor + std::min<unsigned>(
                                       combChunkNodes,
                                       group.expressionIds.size() - cursor);
      for (const SimStatement &statement : chunk) {
        if (cursor >= chunkEnd)
          return failure();
        if (statement.kind == SimStatementKind::Expression) {
          if (statement.expressionId != group.expressionIds[cursor] ||
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
          if (expr.kind != SimExprKind::Mux || expr.operands.size() != 3 ||
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
  for (unsigned target : packedActivationGroups) {
    if (!groupByNode.count(target) || !groupActivations.count(target))
      return failure();
    for (unsigned input : graph.topNodes[target].inputIds) {
      bool covered = false;
      for (const SimEdge &edge : graph.edges)
        if (edge.consumer == target && edge.valueId == input &&
            groupByNode.count(edge.producer) &&
            combPosition.count(edge.producer) && combPosition.count(target) &&
            combPosition.lookup(edge.producer) < combPosition.lookup(target))
          covered = true;
      if (!covered)
        return failure();
    }
  }
  std::set<std::tuple<unsigned, unsigned, unsigned>> expectedPropagation;
  for (const SimEdge &edge : graph.edges)
    if (groupByNode.count(edge.producer) &&
        packedActivationGroups.contains(edge.consumer))
      expectedPropagation.emplace(edge.producer, edge.consumer, edge.valueId);
  std::set<std::tuple<unsigned, unsigned, unsigned>> actualPropagation;
  for (const auto &entry : groupPropagations) {
    if (!groupByNode.count(entry.first))
      return failure();
    for (const GroupPropagationTarget &target : entry.second) {
      if (!packedActivationGroups.contains(target.groupNodeId) ||
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
  llvm::DenseSet<unsigned> batchedRegs;
  for (const ResetGroupPlan &group : resetGroups) {
    if (group.regNodeIds.size() < 2 ||
        group.clockValueId >= graph.values.size() ||
        group.resetValueId >= graph.values.size())
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
    if (id >= graph.values.size() || !graph.values[id].type.shape.empty() ||
        graph.values[id].type.width > 64)
      return failure();
  for (const auto &entry : instanceCaches)
    if (entry.first >= graph.topNodes.size() ||
        graph.topNodes[entry.first].kind != SimNodeKind::Instance)
      return failure();
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
      for (unsigned inputId : node.inputIds) {
        unsigned width = plan.graph.values[inputId].type.width;
        cache.packedWords += std::max(1u, (width + 63u) / 64u);
        if (width <= 64)
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
      if (plan.graph.values[dataId].type.width <= 64)
        plan.fingerprintInputs.insert(dataId);
      plan.invalidateCachesOnCommit.insert(nodeId);
    }
    if (node.kind == SimNodeKind::AsyncFifo) {
      unsigned dataId = node.inputIds[5];
      if (plan.graph.values[dataId].type.width <= 64)
        plan.fingerprintInputs.insert(dataId);
      plan.invalidateCachesOnCommit.insert(nodeId);
    }
    if (node.kind == SimNodeKind::ByteMem) {
      for (unsigned index : {2u, 4u, 5u}) {
        unsigned valueId = node.inputIds[index];
        if (plan.graph.values[valueId].type.width <= 64)
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
    if (regs.size() >= 2)
      plan.resetGroups.push_back(ResetGroupPlan{key.first, key.second,
                                                std::move(regs)});
  planLocalTickActions(plan, plan.localTickComputeActions,
                       plan.localTickCommitActions);
  DependencyGraph comb;
  if (buildDependencies(plan.graph, names, false, comb))
    plan.combTopological = topoOrder(comb, plan.combNodeOrder);
  if (!plan.combTopological) {
    for (auto [id, node] : llvm::enumerate(plan.graph.topNodes))
      if (node.kind != SimNodeKind::Return && node.kind != SimNodeKind::Wire) {
        plan.combNodeOrder.push_back(static_cast<unsigned>(id));
      }
  }
  planPackedGroupActivation(plan);

  DependencyGraph full;
  if (buildDependencies(plan.graph, names, true, full)) {
    plan.evalTopological = topoOrder(full, plan.evalNodeOrder);
    if (!plan.evalTopological)
      buildSccOrder(full, plan);
  }
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
