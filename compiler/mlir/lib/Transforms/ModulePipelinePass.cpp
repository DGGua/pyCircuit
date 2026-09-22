#include "pyc/Transforms/ModulePipeline.h"

#include "pyc/Dialect/PYC/PYCOps.h"
#include "pyc/Transforms/CombDepGraph.h"
#include "pyc/Transforms/Passes.h"

#include "mlir/IR/BuiltinAttributes.h"
#include "mlir/IR/IRMapping.h"
#include "mlir/IR/SymbolTable.h"
#include "mlir/Interfaces/SideEffectInterfaces.h"
#include "mlir/Pass/Pass.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>
#include <functional>
#include <optional>
#include <string>

using namespace mlir;

namespace pyc {
namespace {

static bool isStateCut(Operation *op) {
  return isa<pyc::RegOp, pyc::FifoOp, pyc::ByteMemOp, pyc::SyncMemOp,
             pyc::SyncMemDPOp, pyc::AsyncFifoOp, pyc::CdcSyncOp>(op);
}

struct SccResult {
  llvm::SmallVector<llvm::SmallVector<unsigned>> components;
};

static SccResult
computeSccs(const llvm::SmallVector<llvm::SmallVector<unsigned>> &edges) {
  SccResult result;
  llvm::SmallVector<int> index(edges.size(), -1);
  llvm::SmallVector<int> lowLink(edges.size(), -1);
  llvm::SmallVector<unsigned> stack;
  llvm::SmallVector<bool> onStack(edges.size(), false);
  int nextIndex = 0;

  std::function<void(unsigned)> visit = [&](unsigned node) {
    index[node] = lowLink[node] = nextIndex++;
    stack.push_back(node);
    onStack[node] = true;
    for (unsigned next : edges[node]) {
      if (index[next] < 0) {
        visit(next);
        lowLink[node] = std::min(lowLink[node], lowLink[next]);
      } else if (onStack[next]) {
        lowLink[node] = std::min(lowLink[node], index[next]);
      }
    }
    if (lowLink[node] != index[node])
      return;
    llvm::SmallVector<unsigned> component;
    while (true) {
      unsigned current = stack.pop_back_val();
      onStack[current] = false;
      component.push_back(current);
      if (current == node)
        break;
    }
    llvm::sort(component);
    result.components.push_back(std::move(component));
  };

  for (unsigned node = 0; node < edges.size(); ++node)
    if (index[node] < 0)
      visit(node);
  return result;
}

static bool
isCyclicComponent(ArrayRef<unsigned> component,
                  const llvm::SmallVector<llvm::SmallVector<unsigned>> &edges) {
  if (component.size() > 1)
    return true;
  unsigned node = component.front();
  return llvm::is_contained(edges[node], node);
}

struct SourcePort {
  Operation *instance = nullptr;
  unsigned result = 0;
};

struct FunctionGraph {
  llvm::SmallVector<pyc::InstanceOp> instances;
  llvm::DenseMap<Operation *, unsigned> instanceIndex;
  llvm::DenseMap<Operation *, unsigned> inputBase;
  llvm::DenseMap<Operation *, unsigned> outputBase;
  llvm::SmallVector<std::string> nodeIds;
  llvm::SmallVector<llvm::SmallVector<unsigned>> portEdges;
  llvm::SmallVector<llvm::SmallVector<unsigned>> coarseEdges;
  llvm::DenseSet<uint64_t> portEdgeSet;
  llvm::DenseSet<uint64_t> coarseEdgeSet;
  int64_t stateCutCount = 0;

  void addPortEdge(unsigned from, unsigned to) {
    uint64_t key = (static_cast<uint64_t>(from) << 32) | to;
    if (portEdgeSet.insert(key).second)
      portEdges[from].push_back(to);
  }

  void addCoarseEdge(unsigned from, unsigned to) {
    uint64_t key = (static_cast<uint64_t>(from) << 32) | to;
    if (coarseEdgeSet.insert(key).second)
      coarseEdges[from].push_back(to);
  }
};

static std::string instanceName(pyc::InstanceOp inst, unsigned ordinal) {
  if (auto name = inst.getNameAttr())
    return name.getValue().str();
  if (auto callee = inst.getCalleeAttr())
    return (callee.getValue() + "_" + llvm::utostr(ordinal)).str();
  return "instance_" + llvm::utostr(ordinal);
}

class LocalSourceCollector {
public:
  explicit LocalSourceCollector(
      llvm::DenseMap<Value, llvm::SmallVector<Value>> &wireDrivers)
      : wireDrivers_(wireDrivers) {}

  void collect(Value value, llvm::SmallVectorImpl<SourcePort> &sources) {
    seen_.clear();
    collectImpl(value, sources);
    llvm::sort(sources, [](const SourcePort &lhs, const SourcePort &rhs) {
      if (lhs.instance != rhs.instance)
        return lhs.instance < rhs.instance;
      return lhs.result < rhs.result;
    });
    sources.erase(std::unique(sources.begin(), sources.end(),
                              [](const SourcePort &lhs, const SourcePort &rhs) {
                                return lhs.instance == rhs.instance &&
                                       lhs.result == rhs.result;
                              }),
                  sources.end());
  }

private:
  void collectImpl(Value value, llvm::SmallVectorImpl<SourcePort> &sources) {
    if (!value || !seen_.insert(value).second)
      return;
    if (auto blockArg = dyn_cast<BlockArgument>(value)) {
      Operation *parent =
          blockArg.getOwner() ? blockArg.getOwner()->getParentOp() : nullptr;
      if (auto comb = dyn_cast_or_null<pyc::CombOp>(parent)) {
        unsigned index = blockArg.getArgNumber();
        if (index < comb.getInputs().size())
          collectImpl(comb.getInputs()[index], sources);
      }
      return;
    }

    Operation *def = value.getDefiningOp();
    if (!def || isStateCut(def))
      return;
    if (auto inst = dyn_cast<pyc::InstanceOp>(def)) {
      auto result = dyn_cast<OpResult>(value);
      if (result)
        sources.push_back({inst.getOperation(), result.getResultNumber()});
      return;
    }
    if (auto wire = dyn_cast<pyc::WireOp>(def)) {
      auto it = wireDrivers_.find(wire.getResult());
      if (it != wireDrivers_.end())
        for (Value driver : it->second)
          collectImpl(driver, sources);
      return;
    }
    if (auto comb = dyn_cast<pyc::CombOp>(def)) {
      auto result = dyn_cast<OpResult>(value);
      if (!result || comb.getBody().empty())
        return;
      auto yield = dyn_cast_or_null<pyc::YieldOp>(
          comb.getBody().front().getTerminator());
      if (yield && result.getResultNumber() < yield.getValues().size())
        collectImpl(yield.getValues()[result.getResultNumber()], sources);
      return;
    }
    for (Value operand : def->getOperands())
      collectImpl(operand, sources);
  }

  llvm::DenseMap<Value, llvm::SmallVector<Value>> &wireDrivers_;
  llvm::DenseSet<Value> seen_;
};

static FailureOr<FunctionGraph>
buildFunctionGraph(ModuleOp module, func::FuncOp func,
                   CombDepGraphCache &combCache) {
  FunctionGraph graph;
  func.walk([&](Operation *op) {
    if (isStateCut(op))
      ++graph.stateCutCount;
  });
  func.walk([&](pyc::InstanceOp inst) {
    graph.instanceIndex.try_emplace(inst.getOperation(),
                                    graph.instances.size());
    graph.instances.push_back(inst);
  });
  graph.coarseEdges.resize(graph.instances.size());

  for (auto [ordinal, inst] : llvm::enumerate(graph.instances)) {
    const std::string name = instanceName(inst, ordinal);
    graph.inputBase.try_emplace(inst.getOperation(), graph.nodeIds.size());
    for (unsigned index = 0; index < inst.getNumOperands(); ++index)
      graph.nodeIds.push_back(name + ".in" + llvm::utostr(index));
    graph.outputBase.try_emplace(inst.getOperation(), graph.nodeIds.size());
    for (unsigned index = 0; index < inst.getNumResults(); ++index)
      graph.nodeIds.push_back(name + ".out" + llvm::utostr(index));
  }
  graph.portEdges.resize(graph.nodeIds.size());

  llvm::DenseMap<Value, llvm::SmallVector<Value>> wireDrivers;
  func.walk([&](pyc::AssignOp assign) {
    Value destination = assign.getDst();
    if (destination && destination.getDefiningOp<pyc::WireOp>())
      wireDrivers[destination].push_back(assign.getSrc());
  });
  for (const auto &entry : wireDrivers) {
    if (entry.second.size() <= 1)
      continue;
    entry.first.getDefiningOp()->emitError(
        "PYC4003 module-pipeline multiple-driver wire is not schedulable");
    return failure();
  }
  LocalSourceCollector sourceCollector(wireDrivers);

  for (pyc::InstanceOp target : graph.instances) {
    unsigned targetIndex = graph.instanceIndex.lookup(target.getOperation());
    for (auto [inputIndex, input] : llvm::enumerate(target.getInputs())) {
      llvm::SmallVector<SourcePort> sources;
      sourceCollector.collect(input, sources);
      unsigned inputNode =
          graph.inputBase.lookup(target.getOperation()) + inputIndex;
      for (const SourcePort &source : sources) {
        auto sourceIt = graph.instanceIndex.find(source.instance);
        if (sourceIt == graph.instanceIndex.end())
          continue;
        auto outputBaseIt = graph.outputBase.find(source.instance);
        auto sourceInst = cast<pyc::InstanceOp>(source.instance);
        if (outputBaseIt == graph.outputBase.end() ||
            source.result >= sourceInst.getNumResults())
          continue;
        graph.addPortEdge(outputBaseIt->second + source.result, inputNode);
        graph.addCoarseEdge(sourceIt->second, targetIndex);
      }
    }
  }

  for (pyc::InstanceOp inst : graph.instances) {
    auto calleeAttr = inst.getCalleeAttr();
    auto callee = calleeAttr
                      ? dyn_cast_or_null<func::FuncOp>(
                            SymbolTable::lookupSymbolIn(module, calleeAttr))
                      : func::FuncOp();
    if (!callee || callee.isDeclaration() || callee.getBody().empty()) {
      inst.emitError("PYC4002 module-pipeline missing-callee-body for ")
          << (calleeAttr ? calleeAttr.getValue() : "<unknown>");
      return failure();
    }
    const FuncCombSummary *summary = combCache.getFuncSummary(callee);
    if (!summary) {
      inst.emitError("PYC4002 module-pipeline failed to compute callee "
                     "dependency summary for ")
          << calleeAttr.getValue();
      return failure();
    }
    for (unsigned resultIndex = 0; resultIndex < summary->results.size() &&
                                   resultIndex < inst.getNumResults();
         ++resultIndex) {
      const llvm::BitVector &dependencies =
          summary->results[resultIndex].argDeps;
      unsigned outputNode =
          graph.outputBase.lookup(inst.getOperation()) + resultIndex;
      for (int inputIndex = dependencies.find_first(); inputIndex >= 0;
           inputIndex = dependencies.find_next(inputIndex)) {
        if (static_cast<unsigned>(inputIndex) >= inst.getNumOperands())
          continue;
        unsigned inputNode = graph.inputBase.lookup(inst.getOperation()) +
                             static_cast<unsigned>(inputIndex);
        graph.addPortEdge(inputNode, outputNode);
      }
    }
  }

  for (auto &edges : graph.portEdges)
    llvm::sort(edges);
  for (auto &edges : graph.coarseEdges)
    llvm::sort(edges);
  return graph;
}

static ArrayAttr buildNodeAttrs(MLIRContext *context,
                                const FunctionGraph &graph) {
  Builder builder(context);
  llvm::SmallVector<Attribute> nodes;
  nodes.reserve(graph.nodeIds.size());
  for (auto [index, id] : llvm::enumerate(graph.nodeIds)) {
    llvm::SmallVector<NamedAttribute> fields;
    fields.push_back(builder.getNamedAttr("id", builder.getStringAttr(id)));
    fields.push_back(builder.getNamedAttr(
        "order", builder.getI64IntegerAttr(static_cast<int64_t>(index))));
    nodes.push_back(builder.getDictionaryAttr(fields));
  }
  return builder.getArrayAttr(nodes);
}

static ArrayAttr buildEdgeAttrs(MLIRContext *context,
                                const FunctionGraph &graph) {
  Builder builder(context);
  llvm::SmallVector<std::pair<unsigned, unsigned>> sortedEdges;
  for (auto [from, successors] : llvm::enumerate(graph.portEdges))
    for (unsigned to : successors)
      sortedEdges.emplace_back(from, to);
  llvm::sort(sortedEdges);

  llvm::SmallVector<Attribute> edges;
  edges.reserve(sortedEdges.size());
  for (auto [from, to] : sortedEdges) {
    llvm::SmallVector<NamedAttribute> fields;
    fields.push_back(builder.getNamedAttr(
        "from", builder.getStringAttr(graph.nodeIds[from])));
    fields.push_back(
        builder.getNamedAttr("to", builder.getStringAttr(graph.nodeIds[to])));
    edges.push_back(builder.getDictionaryAttr(fields));
  }
  return builder.getArrayAttr(edges);
}

static DictionaryAttr buildFunctionMetadata(MLIRContext *context,
                                            const FunctionGraph &graph,
                                            int64_t coarseSccCount,
                                            int64_t falseSccCount,
                                            StringRef mode, bool rewritten) {
  Builder builder(context);
  llvm::SmallVector<NamedAttribute> fields;
  fields.push_back(builder.getNamedAttr(
      "schema", builder.getStringAttr(kModulePipelineSchema)));
  fields.push_back(builder.getNamedAttr("mode", builder.getStringAttr(mode)));
  fields.push_back(
      builder.getNamedAttr("rewritten", builder.getBoolAttr(rewritten)));
  fields.push_back(builder.getNamedAttr(
      "coarse_scc_count", builder.getI64IntegerAttr(coarseSccCount)));
  fields.push_back(builder.getNamedAttr(
      "false_scc_count", builder.getI64IntegerAttr(falseSccCount)));
  fields.push_back(builder.getNamedAttr(
      "state_cut_count", builder.getI64IntegerAttr(graph.stateCutCount)));
  fields.push_back(
      builder.getNamedAttr("nodes", buildNodeAttrs(context, graph)));
  fields.push_back(
      builder.getNamedAttr("edges", buildEdgeAttrs(context, graph)));
  return builder.getDictionaryAttr(fields);
}

struct StagePlan {
  func::FuncOp origin;
  func::FuncOp stage;
  llvm::SmallVector<unsigned> keptArguments;
};

static int64_t countStateOps(ModuleOp module) {
  int64_t count = 0;
  module.walk([&](Operation *op) {
    if (isStateCut(op))
      ++count;
  });
  return count;
}

static llvm::DenseMap<Operation *, Operation *>
snapshotStateOwners(ModuleOp module) {
  llvm::DenseMap<Operation *, Operation *> owners;
  module.walk([&](Operation *op) {
    if (isStateCut(op))
      owners.try_emplace(op,
                         op->getParentOfType<func::FuncOp>().getOperation());
  });
  return owners;
}

static LogicalResult verifyStateOwners(
    ModuleOp module,
    const llvm::DenseMap<Operation *, Operation *> &expectedOwners) {
  int64_t seen = 0;
  LogicalResult result = success();
  module.walk([&](Operation *op) {
    if (!isStateCut(op) || failed(result))
      return;
    ++seen;
    auto expected = expectedOwners.find(op);
    auto owner = op->getParentOfType<func::FuncOp>();
    if (expected == expectedOwners.end() || !owner ||
        expected->second != owner.getOperation()) {
      op->emitError(
          "PYC4005 module-pipeline rewrite invariant: state ownership changed");
      result = failure();
    }
  });
  if (failed(result))
    return failure();
  if (seen != static_cast<int64_t>(expectedOwners.size())) {
    module.emitError(
        "PYC4005 module-pipeline rewrite invariant: state identity changed");
    return failure();
  }
  return success();
}

static LogicalResult emitUnsupported(Operation *op, Twine detail) {
  op->emitError("PYC4004 module-pipeline unsupported-edge: ") << detail;
  return failure();
}

static LogicalResult validateStageCandidate(func::FuncOp callee) {
  if (!callee || callee.isDeclaration() || callee.getBody().empty())
    return emitUnsupported(callee, "stage callee must have a body");
  if (!llvm::hasSingleElement(callee.getBody()))
    return emitUnsupported(callee,
                           "only single-block stage callees are supported");
  if (callee->hasAttr("pyc.probe_only"))
    return emitUnsupported(callee, "probe functions cannot be staged");

  LogicalResult result = success();
  callee.walk([&](Operation *op) {
    if (failed(result) || op == callee.getOperation() ||
        isa<func::ReturnOp, pyc::YieldOp>(op))
      return;
    if (isStateCut(op)) {
      result = emitUnsupported(
          op, "stateful stage extraction is not supported in rewrite v1");
      return;
    }
    if (isa<pyc::InstanceOp, pyc::AssertOp>(op)) {
      result = emitUnsupported(
          op, "nested instances/assertions are not supported in rewrite v1");
      return;
    }
    if (!isMemoryEffectFree(op))
      result = emitUnsupported(
          op, "operation has an unknown or observable side effect");
  });
  return result;
}

static FailureOr<StagePlan>
extractSingleStage(ModuleOp module, func::FuncOp callee,
                   llvm::DenseMap<Operation *, unsigned> &callsiteCounts) {
  if (callsiteCounts.lookup(callee.getOperation()) != 1u) {
    (void)emitUnsupported(callee,
                          "callee must have exactly one pyc.instance callsite");
    return failure();
  }
  if (failed(validateStageCandidate(callee)))
    return failure();

  StagePlan plan;
  plan.origin = callee;
  llvm::SmallVector<Type> stageInputs;
  for (BlockArgument argument : callee.getArguments()) {
    if (argument.use_empty())
      continue;
    plan.keptArguments.push_back(argument.getArgNumber());
    stageInputs.push_back(argument.getType());
  }

  std::string stageName = (callee.getSymName() + "__pyc_stage_0").str();
  if (SymbolTable::lookupSymbolIn(module, stageName)) {
    (void)emitUnsupported(
        callee, "generated stage symbol already exists without v1 metadata");
    return failure();
  }

  auto stageType = FunctionType::get(module.getContext(), stageInputs,
                                     callee.getFunctionType().getResults());
  func::FuncOp stage =
      func::FuncOp::create(callee.getLoc(), stageName, stageType);
  for (NamedAttribute attribute : callee->getAttrs()) {
    StringRef name = attribute.getName().strref();
    if (name == SymbolTable::getSymbolAttrName() || name == "function_type" ||
        name == "arg_attrs" || name == "res_attrs" || name == "arg_names")
      continue;
    stage->setAttr(attribute.getName(), attribute.getValue());
  }

  Builder attrBuilder(module.getContext());
  if (auto originalArgAttrs = callee->getAttrOfType<ArrayAttr>("arg_attrs")) {
    llvm::SmallVector<Attribute> argAttrs;
    argAttrs.reserve(plan.keptArguments.size());
    for (unsigned index : plan.keptArguments)
      if (index < originalArgAttrs.size())
        argAttrs.push_back(originalArgAttrs[index]);
    if (argAttrs.size() == plan.keptArguments.size())
      stage->setAttr("arg_attrs", attrBuilder.getArrayAttr(argAttrs));
  }
  if (auto resultAttrs = callee->getAttrOfType<ArrayAttr>("res_attrs"))
    stage->setAttr("res_attrs", resultAttrs);

  llvm::SmallVector<Attribute> argNames;
  if (auto originalNames = callee->getAttrOfType<ArrayAttr>("arg_names")) {
    for (unsigned index : plan.keptArguments)
      if (index < originalNames.size())
        argNames.push_back(originalNames[index]);
  }
  if (argNames.size() == plan.keptArguments.size())
    stage->setAttr("arg_names", attrBuilder.getArrayAttr(argNames));
  stage->setAttr(kPipelineGeneratedAttr, attrBuilder.getBoolAttr(true));
  stage->setAttr(kPipelineOriginAttr,
                 attrBuilder.getStringAttr(callee.getSymName()));
  stage->setAttr(kPipelineStageAttr, attrBuilder.getI64IntegerAttr(0));
  stage->setAttr(kPipelineLogicalPathAttr,
                 attrBuilder.getStringAttr(callee.getSymName()));
  stage->setAttr(kPipelineValidatedAttr,
                 attrBuilder.getStringAttr(kModulePipelineSchema));

  Block *entry = stage.addEntryBlock();
  IRMapping mapping;
  for (auto [newIndex, oldIndex] : llvm::enumerate(plan.keptArguments))
    mapping.map(callee.getArgument(oldIndex), entry->getArgument(newIndex));
  OpBuilder bodyBuilder = OpBuilder::atBlockEnd(entry);
  for (Operation &op : callee.getBody().front())
    bodyBuilder.clone(op, mapping);

  module.push_back(stage);
  plan.stage = stage;
  return plan;
}

static LogicalResult rewriteCallsite(pyc::InstanceOp instance,
                                     const StagePlan &plan) {
  llvm::SmallVector<Value> inputs;
  inputs.reserve(plan.keptArguments.size());
  for (unsigned index : plan.keptArguments) {
    if (index >= instance.getNumOperands())
      return emitUnsupported(instance, "stage argument index is out of range");
    inputs.push_back(instance.getOperand(index));
  }

  NamedAttrList attrs(instance->getAttrs());
  auto stageSymbol =
      plan.stage->getAttrOfType<StringAttr>(SymbolTable::getSymbolAttrName());
  attrs.set("callee", FlatSymbolRefAttr::get(instance.getContext(),
                                             stageSymbol.getValue()));
  std::string logicalPath = instanceName(instance, 0);
  attrs.set(kPipelineLogicalPathAttr,
            StringAttr::get(instance.getContext(), logicalPath));

  OpBuilder builder(instance);
  OperationState state(instance.getLoc(), pyc::InstanceOp::getOperationName());
  state.addOperands(inputs);
  state.addTypes(instance.getResultTypes());
  state.addAttributes(attrs);
  Operation *replacement = builder.create(state);
  instance->replaceAllUsesWith(replacement->getResults());
  instance.erase();
  return success();
}

static LogicalResult verifyRewrittenModule(ModuleOp module,
                                           int64_t expectedStateCount,
                                           int64_t expectedStages) {
  if (countStateOps(module) != expectedStateCount) {
    module.emitError(
        "PYC4005 module-pipeline rewrite invariant: state count changed");
    return failure();
  }

  int64_t generatedStages = 0;
  for (func::FuncOp func : module.getOps<func::FuncOp>()) {
    if (!isPipelineGeneratedFunc(func))
      continue;
    ++generatedStages;
    if (!func->getAttrOfType<StringAttr>(kPipelineOriginAttr) ||
        !func->getAttrOfType<IntegerAttr>(kPipelineStageAttr) ||
        !func->getAttrOfType<StringAttr>(kPipelineLogicalPathAttr) ||
        !isPipelineValidatedFunc(func)) {
      func.emitError("PYC4005 module-pipeline rewrite invariant: incomplete "
                     "stage metadata");
      return failure();
    }
  }
  if (generatedStages != expectedStages) {
    module.emitError("PYC4005 module-pipeline rewrite invariant: generated "
                     "stage count mismatch");
    return failure();
  }

  CombDepGraphCache cache(module);
  for (func::FuncOp func : module.getOps<func::FuncOp>()) {
    if (func.isDeclaration())
      continue;
    FailureOr<FunctionGraph> graph = buildFunctionGraph(module, func, cache);
    if (failed(graph))
      return failure();
    for (const auto &component : computeSccs(graph->coarseEdges).components) {
      if (!isCyclicComponent(component, graph->coarseEdges))
        continue;
      func.emitError("PYC4005 module-pipeline rewrite invariant: instance DAG "
                     "remains cyclic");
      return failure();
    }
  }
  return success();
}

static DictionaryAttr
buildModuleSummaryMetadata(MLIRContext *context,
                           const ModulePipelineSummary &summary) {
  Builder builder(context);
  llvm::SmallVector<NamedAttribute> fields;
  fields.push_back(builder.getNamedAttr(
      "schema", builder.getStringAttr(kModulePipelineSchema)));
  fields.push_back(builder.getNamedAttr(
      "mode",
      builder.getStringAttr(summary.rewritten ? "rewrite" : "analysis")));
  fields.push_back(builder.getNamedAttr(
      "rewritten", builder.getBoolAttr(summary.rewritten)));
  fields.push_back(builder.getNamedAttr(
      "function_count", builder.getI64IntegerAttr(summary.functionCount)));
  fields.push_back(builder.getNamedAttr(
      "coarse_scc_count",
      builder.getI64IntegerAttr(summary.coarseSccCount)));
  fields.push_back(builder.getNamedAttr(
      "false_scc_count", builder.getI64IntegerAttr(summary.falseSccCount)));
  fields.push_back(builder.getNamedAttr(
      "true_cycle_count",
      builder.getI64IntegerAttr(summary.trueCycleCount)));
  fields.push_back(builder.getNamedAttr(
      "state_cut_count", builder.getI64IntegerAttr(summary.stateCutCount)));
  fields.push_back(builder.getNamedAttr(
      "generated_stage_count",
      builder.getI64IntegerAttr(summary.generatedStageCount)));
  fields.push_back(builder.getNamedAttr(
      "minimum_stage_count",
      builder.getI64IntegerAttr(summary.generatedStageCount)));
  return builder.getDictionaryAttr(fields);
}

static FailureOr<ModulePipelineSummary>
recomputeRewrittenMetadata(ModuleOp module, int64_t generatedStageCount,
                           bool verifyExisting) {
  ModulePipelineSummary summary;
  summary.generatedStageCount = generatedStageCount;
  summary.rewritten = true;
  CombDepGraphCache cache(module);

  for (func::FuncOp func : module.getOps<func::FuncOp>()) {
    if (func.isDeclaration())
      continue;
    ++summary.functionCount;
    FailureOr<FunctionGraph> graph = buildFunctionGraph(module, func, cache);
    if (failed(graph))
      return failure();
    summary.stateCutCount += graph->stateCutCount;

    int64_t coarseSccCount = 0;
    for (const auto &component : computeSccs(graph->coarseEdges).components)
      if (isCyclicComponent(component, graph->coarseEdges))
        ++coarseSccCount;
    int64_t trueCycleCount = 0;
    for (const auto &component : computeSccs(graph->portEdges).components)
      if (isCyclicComponent(component, graph->portEdges))
        ++trueCycleCount;
    if (trueCycleCount != 0) {
      func.emitError("PYC4005 module-pipeline rewrite invariant: "
                     "post-rewrite graph contains a true cycle");
      return failure();
    }

    summary.coarseSccCount += coarseSccCount;
    summary.falseSccCount += coarseSccCount;
    DictionaryAttr expected =
        buildFunctionMetadata(module.getContext(), *graph, coarseSccCount,
                              coarseSccCount, "rewrite", true);
    if (verifyExisting) {
      if (!isPipelineValidatedFunc(func) ||
          func->getAttrOfType<DictionaryAttr>(kPipelineStageDagAttr) !=
              expected) {
        func.emitError("PYC4005 module-pipeline rewrite invariant: stale "
                       "function graph metadata");
        return failure();
      }
    } else {
      func->setAttr(
          kPipelineValidatedAttr,
          StringAttr::get(module.getContext(), kModulePipelineSchema));
      func->setAttr(kPipelineStageDagAttr, expected);
    }
  }
  return summary;
}

struct ModulePipelinePass
    : public PassWrapper<ModulePipelinePass, OperationPass<ModuleOp>> {
  MLIR_DEFINE_EXPLICIT_INTERNAL_INLINE_TYPE_ID(ModulePipelinePass)

  ModulePipelinePass() = default;
  explicit ModulePipelinePass(bool rewrite) : rewrite_(rewrite) {}

  StringRef getArgument() const override { return "pyc-module-pipeline"; }
  StringRef getDescription() const override {
    return "Classify whole-instance SCCs with port-level dependencies and "
           "attach a deterministic module stage DAG";
  }

  void runOnOperation() override {
    ModuleOp module = getOperation();
    if (auto existing =
            module->getAttrOfType<DictionaryAttr>(kModulePipelineSummaryAttr)) {
      auto schema = existing.getAs<StringAttr>("schema");
      if (!schema || schema.getValue() != kModulePipelineSchema) {
        module.emitError("PYC4005 unknown module-pipeline metadata schema");
        signalPassFailure();
        return;
      }
      bool wasRewritten = false;
      if (auto value = existing.getAs<BoolAttr>("rewritten"))
        wasRewritten = value.getValue();
      auto existingMode = existing.getAs<StringAttr>("mode");
      StringRef expectedMode = wasRewritten ? "rewrite" : "analysis";
      if (!existingMode || existingMode.getValue() != expectedMode) {
        module.emitError("PYC4005 inconsistent module-pipeline mode metadata");
        signalPassFailure();
        return;
      }
      if (!rewrite_ || wasRewritten) {
        if (rewrite_) {
          auto stateCount = existing.getAs<IntegerAttr>("state_cut_count");
          auto stageCount =
              existing.getAs<IntegerAttr>("generated_stage_count");
          if (!stateCount || !stageCount) {
            module.emitError(
                "PYC4005 incomplete module-pipeline rewrite metadata");
            signalPassFailure();
            return;
          }
          if (failed(verifyRewrittenModule(module, stateCount.getInt(),
                                           stageCount.getInt())))
            signalPassFailure();
          else {
            FailureOr<ModulePipelineSummary> recomputed =
                recomputeRewrittenMetadata(module, stageCount.getInt(), true);
            if (failed(recomputed) ||
                buildModuleSummaryMetadata(module.getContext(), *recomputed) !=
                    existing) {
              if (succeeded(recomputed))
                module.emitError("PYC4005 module-pipeline rewrite invariant: "
                                 "stale module graph metadata");
              signalPassFailure();
            }
          }
        }
        return;
      }
      // Analysis metadata is deliberately replaceable by the physical rewrite.
      module->removeAttr(kModulePipelineSummaryAttr);
      for (func::FuncOp func : module.getOps<func::FuncOp>()) {
        func->removeAttr(kPipelineValidatedAttr);
        func->removeAttr(kPipelineStageDagAttr);
      }
    }

    CombDepGraphCache combCache(module);
    ModulePipelineSummary aggregate;
    llvm::DenseSet<Operation *> rewriteCallees;
    llvm::DenseMap<Operation *, Type> originalSignatures;
    const int64_t stateCountBefore = countStateOps(module);
    const auto stateOwnersBefore = snapshotStateOwners(module);

    for (func::FuncOp func : module.getOps<func::FuncOp>()) {
      if (func.isDeclaration())
        continue;
      originalSignatures.try_emplace(func.getOperation(),
                                     func.getFunctionType());
      ++aggregate.functionCount;
      FailureOr<FunctionGraph> graphOr =
          buildFunctionGraph(module, func, combCache);
      if (failed(graphOr)) {
        signalPassFailure();
        return;
      }
      FunctionGraph &graph = *graphOr;
      aggregate.stateCutCount += graph.stateCutCount;

      int64_t coarseSccCount = 0;
      llvm::SmallVector<llvm::SmallVector<unsigned>> coarseCycles;
      for (const auto &component : computeSccs(graph.coarseEdges).components) {
        if (!isCyclicComponent(component, graph.coarseEdges))
          continue;
        ++coarseSccCount;
        coarseCycles.push_back(component);
      }
      aggregate.coarseSccCount += coarseSccCount;

      llvm::SmallVector<llvm::SmallVector<unsigned>> trueCycles;
      for (const auto &component : computeSccs(graph.portEdges).components)
        if (isCyclicComponent(component, graph.portEdges))
          trueCycles.push_back(component);
      if (!trueCycles.empty()) {
        aggregate.trueCycleCount += trueCycles.size();
        llvm::SmallVector<std::string> labels;
        for (unsigned node : trueCycles.front())
          labels.push_back(graph.nodeIds[node]);
        llvm::sort(labels);
        std::string message;
        llvm::raw_string_ostream os(message);
        os << "PYC4001 cross-instance combinational cycle";
        for (const std::string &label : labels)
          os << (label == labels.front() ? ": " : " -> ") << label;
        func.emitError(os.str());
        signalPassFailure();
        return;
      }

      // With no port-level cycle, every cycle introduced by whole-instance
      // atomicity is a false SCC.
      int64_t falseSccCount = coarseSccCount;
      aggregate.falseSccCount += falseSccCount;
      if (rewrite_) {
        for (const auto &component : coarseCycles) {
          for (unsigned instanceIndex : component) {
            pyc::InstanceOp instance = graph.instances[instanceIndex];
            auto callee = dyn_cast_or_null<func::FuncOp>(
                SymbolTable::lookupSymbolIn(module, instance.getCalleeAttr()));
            if (callee)
              rewriteCallees.insert(callee.getOperation());
          }
        }
      } else {
        func->setAttr(
            kPipelineValidatedAttr,
            StringAttr::get(module.getContext(), kModulePipelineSchema));
        func->setAttr(kPipelineStageDagAttr,
                      buildFunctionMetadata(module.getContext(), graph,
                                            coarseSccCount, falseSccCount,
                                            "analysis", false));
      }
    }

    if (rewrite_) {
      llvm::DenseMap<Operation *, unsigned> callsiteCounts;
      module.walk([&](pyc::InstanceOp instance) {
        auto callee = dyn_cast_or_null<func::FuncOp>(
            SymbolTable::lookupSymbolIn(module, instance.getCalleeAttr()));
        if (callee)
          ++callsiteCounts[callee.getOperation()];
      });

      llvm::SmallVector<StagePlan> plans;
      llvm::DenseMap<Operation *, unsigned> planIndex;
      llvm::SmallVector<func::FuncOp> originalFunctions;
      for (func::FuncOp func : module.getOps<func::FuncOp>())
        originalFunctions.push_back(func);
      for (func::FuncOp func : originalFunctions) {
        if (!rewriteCallees.contains(func.getOperation()))
          continue;
        FailureOr<StagePlan> plan =
            extractSingleStage(module, func, callsiteCounts);
        if (failed(plan)) {
          signalPassFailure();
          return;
        }
        planIndex.try_emplace(func.getOperation(), plans.size());
        plans.push_back(std::move(*plan));
      }

      llvm::SmallVector<pyc::InstanceOp> callsites;
      module.walk([&](pyc::InstanceOp instance) {
        auto callee = dyn_cast_or_null<func::FuncOp>(
            SymbolTable::lookupSymbolIn(module, instance.getCalleeAttr()));
        if (callee && planIndex.contains(callee.getOperation()))
          callsites.push_back(instance);
      });
      for (pyc::InstanceOp instance : callsites) {
        auto callee = dyn_cast<func::FuncOp>(
            SymbolTable::lookupSymbolIn(module, instance.getCalleeAttr()));
        if (failed(rewriteCallsite(
                instance, plans[planIndex.lookup(callee.getOperation())]))) {
          signalPassFailure();
          return;
        }
      }
      aggregate.generatedStageCount = plans.size();

      for (const auto &entry : originalSignatures) {
        auto func = dyn_cast<func::FuncOp>(entry.first);
        if (!func || func.getFunctionType() != entry.second) {
          module.emitError("PYC4005 module-pipeline rewrite invariant: "
                           "original signature changed");
          signalPassFailure();
          return;
        }
      }
      if (failed(verifyStateOwners(module, stateOwnersBefore))) {
        signalPassFailure();
        return;
      }
      if (failed(verifyRewrittenModule(module, stateCountBefore,
                                       aggregate.generatedStageCount))) {
        signalPassFailure();
        return;
      }

      FailureOr<ModulePipelineSummary> rewritten =
          recomputeRewrittenMetadata(module, aggregate.generatedStageCount,
                                     false);
      if (failed(rewritten)) {
        signalPassFailure();
        return;
      }
      aggregate = *rewritten;
    }

    aggregate.rewritten = rewrite_;
    module->setAttr(kModulePipelineSummaryAttr,
                    buildModuleSummaryMetadata(module.getContext(), aggregate));
  }

private:
  bool rewrite_ = false;
};

} // namespace

bool isPipelineGeneratedFunc(func::FuncOp func) {
  auto generated = func->getAttrOfType<BoolAttr>(kPipelineGeneratedAttr);
  return generated && generated.getValue();
}

bool isPipelineValidatedFunc(func::FuncOp func) {
  auto schema = func->getAttrOfType<StringAttr>(kPipelineValidatedAttr);
  return schema && schema.getValue() == kModulePipelineSchema;
}

std::optional<ModulePipelineSummary>
readModulePipelineSummary(ModuleOp module) {
  auto attr = module->getAttrOfType<DictionaryAttr>(kModulePipelineSummaryAttr);
  if (!attr)
    return std::nullopt;
  auto schema = attr.getAs<StringAttr>("schema");
  if (!schema || schema.getValue() != kModulePipelineSchema)
    return std::nullopt;

  ModulePipelineSummary summary;
  auto readInteger = [&](StringRef name) -> int64_t {
    if (auto value = attr.getAs<IntegerAttr>(name))
      return value.getInt();
    return 0;
  };
  summary.functionCount = readInteger("function_count");
  summary.coarseSccCount = readInteger("coarse_scc_count");
  summary.falseSccCount = readInteger("false_scc_count");
  summary.trueCycleCount = readInteger("true_cycle_count");
  summary.stateCutCount = readInteger("state_cut_count");
  summary.generatedStageCount = readInteger("generated_stage_count");
  if (auto rewritten = attr.getAs<BoolAttr>("rewritten"))
    summary.rewritten = rewritten.getValue();
  return summary;
}

std::unique_ptr<::mlir::Pass> createModulePipelinePass(bool rewrite) {
  return std::make_unique<ModulePipelinePass>(rewrite);
}

static PassRegistration<ModulePipelinePass> pass;

} // namespace pyc
