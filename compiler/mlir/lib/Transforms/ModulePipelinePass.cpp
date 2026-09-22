#include "pyc/Transforms/ModulePipeline.h"

#include "pyc/Dialect/PYC/PYCOps.h"
#include "pyc/Transforms/CombDepGraph.h"
#include "pyc/Transforms/Passes.h"

#include "mlir/IR/BuiltinAttributes.h"
#include "mlir/IR/SymbolTable.h"
#include "mlir/Pass/Pass.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>
#include <functional>
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
                                            int64_t falseSccCount) {
  Builder builder(context);
  llvm::SmallVector<NamedAttribute> fields;
  fields.push_back(builder.getNamedAttr(
      "schema", builder.getStringAttr(kModulePipelineSchema)));
  fields.push_back(
      builder.getNamedAttr("mode", builder.getStringAttr("analysis-only")));
  fields.push_back(
      builder.getNamedAttr("rewritten", builder.getBoolAttr(false)));
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

struct ModulePipelinePass
    : public PassWrapper<ModulePipelinePass, OperationPass<ModuleOp>> {
  MLIR_DEFINE_EXPLICIT_INTERNAL_INLINE_TYPE_ID(ModulePipelinePass)

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
      }
      return;
    }

    CombDepGraphCache combCache(module);
    ModulePipelineSummary aggregate;

    for (func::FuncOp func : module.getOps<func::FuncOp>()) {
      if (func.isDeclaration())
        continue;
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
      for (const auto &component : computeSccs(graph.coarseEdges).components)
        if (isCyclicComponent(component, graph.coarseEdges))
          ++coarseSccCount;
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
      // atomicity is a false SCC. Physical func extraction is intentionally
      // deferred until state ownership and trace-path rewriting are proven.
      int64_t falseSccCount = coarseSccCount;
      aggregate.falseSccCount += falseSccCount;
      func->setAttr(
          kPipelineValidatedAttr,
          StringAttr::get(module.getContext(), kModulePipelineSchema));
      func->setAttr(kPipelineStageDagAttr,
                    buildFunctionMetadata(module.getContext(), graph,
                                          coarseSccCount, falseSccCount));
    }

    Builder builder(module.getContext());
    llvm::SmallVector<NamedAttribute> fields;
    fields.push_back(builder.getNamedAttr(
        "schema", builder.getStringAttr(kModulePipelineSchema)));
    fields.push_back(
        builder.getNamedAttr("mode", builder.getStringAttr("analysis-only")));
    fields.push_back(
        builder.getNamedAttr("rewritten", builder.getBoolAttr(false)));
    fields.push_back(builder.getNamedAttr(
        "function_count", builder.getI64IntegerAttr(aggregate.functionCount)));
    fields.push_back(builder.getNamedAttr(
        "coarse_scc_count",
        builder.getI64IntegerAttr(aggregate.coarseSccCount)));
    fields.push_back(builder.getNamedAttr(
        "false_scc_count", builder.getI64IntegerAttr(aggregate.falseSccCount)));
    fields.push_back(builder.getNamedAttr(
        "true_cycle_count",
        builder.getI64IntegerAttr(aggregate.trueCycleCount)));
    fields.push_back(builder.getNamedAttr(
        "state_cut_count", builder.getI64IntegerAttr(aggregate.stateCutCount)));
    module->setAttr(kModulePipelineSummaryAttr,
                    builder.getDictionaryAttr(fields));
  }
};

} // namespace

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
  if (auto rewritten = attr.getAs<BoolAttr>("rewritten"))
    summary.rewritten = rewritten.getValue();
  return summary;
}

std::unique_ptr<::mlir::Pass> createModulePipelinePass() {
  return std::make_unique<ModulePipelinePass>();
}

static PassRegistration<ModulePipelinePass> pass;

} // namespace pyc
