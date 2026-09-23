#include "pyc/Transforms/Passes.h"

#include "pyc/Dialect/PYC/PYCOps.h"
#include "pyc/Transforms/CombPartition.h"
#include "pyc/Transforms/FusedCombPartition.h"

#include "mlir/Dialect/Func/IR/FuncOps.h"
#include "mlir/IR/Builders.h"
#include "mlir/IR/IRMapping.h"
#include "mlir/Pass/Pass.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/Support/CommandLine.h"

#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

using namespace mlir;

namespace pyc {
namespace {

struct FrozenCombPlan {
  CombOp comb;
  std::unique_ptr<FusedCombDepGraph> graph;
  FusedCombPartitionPlan partition;
  llvm::SmallVector<StringAttr> resultNames;
  StringAttr wrapperName;
  uint64_t parentId = 0;
};

struct OutputSpec {
  Value bodyValue;
  int64_t originalResult = -1;
};

static bool hasPartitionMetadata(CombOp comb) {
  return comb->hasAttr(kCombPartitionParentIdAttr) ||
         comb->hasAttr(kCombPartitionPartIdAttr) ||
         comb->hasAttr(kCombPartitionPartCountAttr) ||
         comb->hasAttr(kCombPartitionPlanVersionAttr) ||
         comb->hasAttr(kCombPartitionWorkAttr) ||
         comb->hasAttr(kCombPartitionMaxNodesAttr);
}

static LogicalResult
readObservableNames(CombOp comb, llvm::SmallVectorImpl<StringAttr> &resultNames,
                    StringAttr &wrapperName) {
  resultNames.assign(comb.getNumResults(), StringAttr());
  if (Attribute raw = comb->getAttr(kCombResultNamesAttr)) {
    auto names = dyn_cast<ArrayAttr>(raw);
    if (!names)
      return comb.emitOpError("'")
             << kCombResultNamesAttr << "' must be an array of strings";
    if (names.size() != comb.getNumResults())
      return comb.emitOpError("'")
             << kCombResultNamesAttr << "' length must match result arity";
    for (auto [index, attribute] : llvm::enumerate(names)) {
      auto name = dyn_cast<StringAttr>(attribute);
      if (!name)
        return comb.emitOpError("'") << kCombResultNamesAttr << "' entry "
                                     << index << " must be a string";
      resultNames[index] = name;
    }
  }
  if (comb.getNumResults() == 1)
    wrapperName = comb->getAttrOfType<StringAttr>("pyc.name");
  return success();
}

static void stampLocalPartition(CombOp comb, uint64_t parentId, uint64_t partId,
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
                StringAttr::get(context, kFusedCombPartitionPlanVersion));
  comb->setAttr(kCombPartitionWorkAttr, integer(work));
  comb->setAttr(kCombPartitionMaxNodesAttr, integer(maxNodes));
}

static LogicalResult materialize(FrozenCombPlan &frozen, unsigned maxNodes) {
  CombOp original = frozen.comb;
  Block &originalBody = original.getBody().front();
  auto originalYield = cast<YieldOp>(originalBody.getTerminator());
  ArrayRef<Operation *> operations = frozen.graph->getOperations();
  const unsigned partCount = frozen.partition.ends.size();

  llvm::DenseMap<Operation *, unsigned> nodeForOperation;
  for (auto [node, operation] : llvm::enumerate(operations))
    nodeForOperation[operation] = static_cast<unsigned>(node);

  std::vector<unsigned> nodePart(operations.size());
  unsigned begin = 0;
  for (auto [part, end] : llvm::enumerate(frozen.partition.ends)) {
    for (unsigned offset = begin; offset < end; ++offset)
      nodePart[frozen.partition.order[offset]] = static_cast<unsigned>(part);
    begin = end;
  }

  std::vector<llvm::SmallVector<OutputSpec>> outputs(partCount);
  std::vector<llvm::DenseSet<Value>> seenBoundary(partCount);
  begin = 0;
  for (auto [part, end] : llvm::enumerate(frozen.partition.ends)) {
    for (unsigned offset = begin; offset < end; ++offset) {
      unsigned node = frozen.partition.order[offset];
      for (Value result : operations[node]->getResults()) {
        bool crossesPartition =
            llvm::any_of(result.getUses(), [&](OpOperand &use) {
              if (isa<YieldOp>(use.getOwner()))
                return false;
              auto user = nodeForOperation.find(use.getOwner());
              return user != nodeForOperation.end() &&
                     nodePart[user->second] != part;
            });
        if (crossesPartition && seenBoundary[part].insert(result).second)
          outputs[part].push_back(OutputSpec{result, -1});
      }
    }
    begin = end;
  }

  for (auto [resultIndex, yielded] :
       llvm::enumerate(originalYield.getValues())) {
    unsigned ownerPart = partCount - 1;
    if (Operation *definition = yielded.getDefiningOp()) {
      auto node = nodeForOperation.find(definition);
      if (node == nodeForOperation.end()) {
        original.emitOpError(
            "yielded value is not represented in the local dependency graph");
        return failure();
      }
      ownerPart = nodePart[node->second];
    }
    outputs[ownerPart].push_back(
        OutputSpec{yielded, static_cast<int64_t>(resultIndex)});
  }

  IRMapping outerValues;
  for (auto [argument, input] :
       llvm::zip(originalBody.getArguments(), original.getInputs()))
    outerValues.map(argument, input);
  llvm::SmallVector<Value> replacements(original.getNumResults());

  begin = 0;
  for (auto [partId, end] : llvm::enumerate(frozen.partition.ends)) {
    llvm::SmallVector<Value> bodyInputs;
    llvm::DenseSet<Value> seenInputs;
    for (unsigned offset = begin; offset < end; ++offset) {
      unsigned node = frozen.partition.order[offset];
      for (Value operand : operations[node]->getOperands()) {
        Operation *definition = operand.getDefiningOp();
        auto producer = nodeForOperation.find(definition);
        if (producer != nodeForOperation.end() &&
            nodePart[producer->second] == partId)
          continue;
        if (seenInputs.insert(operand).second)
          bodyInputs.push_back(operand);
      }
    }
    for (const OutputSpec &output : outputs[partId]) {
      Operation *definition = output.bodyValue.getDefiningOp();
      auto producer = nodeForOperation.find(definition);
      if (producer != nodeForOperation.end() &&
          nodePart[producer->second] == partId)
        continue;
      if (seenInputs.insert(output.bodyValue).second)
        bodyInputs.push_back(output.bodyValue);
    }

    llvm::SmallVector<Value> inputs;
    inputs.reserve(bodyInputs.size());
    for (Value bodyInput : bodyInputs) {
      Value mapped = outerValues.lookupOrNull(bodyInput);
      if (!mapped) {
        original.emitOpError(
            "local partition input is unavailable from an earlier sibling");
        return failure();
      }
      inputs.push_back(mapped);
    }

    llvm::SmallVector<Type> resultTypes;
    resultTypes.reserve(outputs[partId].size());
    for (const OutputSpec &output : outputs[partId])
      resultTypes.push_back(output.bodyValue.getType());

    OpBuilder builder(original);
    auto partition =
        builder.create<CombOp>(original.getLoc(), resultTypes, inputs);
    stampLocalPartition(partition, frozen.parentId, partId, partCount,
                        end - begin, maxNodes);

    Block *body = new Block();
    partition.getBody().push_back(body);
    for (Value input : inputs)
      body->addArgument(input.getType(), original.getLoc());

    IRMapping localValues;
    for (auto [bodyInput, argument] :
         llvm::zip(bodyInputs, body->getArguments()))
      localValues.map(bodyInput, argument);

    builder.setInsertionPointToStart(body);
    for (unsigned offset = begin; offset < end; ++offset) {
      unsigned node = frozen.partition.order[offset];
      Operation *clone = builder.clone(*operations[node], localValues);
      for (auto [oldResult, newResult] :
           llvm::zip(operations[node]->getResults(), clone->getResults()))
        localValues.map(oldResult, newResult);
    }

    llvm::SmallVector<Value> yielded;
    yielded.reserve(outputs[partId].size());
    for (const OutputSpec &output : outputs[partId]) {
      Value mapped = localValues.lookupOrNull(output.bodyValue);
      if (!mapped) {
        original.emitOpError("failed to map a local partition output");
        return failure();
      }
      yielded.push_back(mapped);
    }
    builder.create<YieldOp>(originalYield.getLoc(), yielded);

    llvm::SmallVector<Attribute> generatedNames(outputs[partId].size(),
                                                builder.getStringAttr(""));
    bool hasGeneratedName = false;
    bool ownsWrapperResult = false;
    for (auto [outputIndex, output] : llvm::enumerate(outputs[partId])) {
      Value result = partition.getResult(outputIndex);
      if (!outerValues.lookupOrNull(output.bodyValue))
        outerValues.map(output.bodyValue, result);
      if (output.originalResult < 0)
        continue;
      unsigned originalResult = static_cast<unsigned>(output.originalResult);
      replacements[originalResult] = result;
      StringAttr name = frozen.resultNames[originalResult];
      if ((!name || name.getValue().empty()) && originalResult == 0)
        name = frozen.wrapperName;
      if (name && !name.getValue().empty()) {
        generatedNames[outputIndex] = name;
        hasGeneratedName = true;
      }
      ownsWrapperResult |= originalResult == 0;
    }
    if (hasGeneratedName)
      partition->setAttr(kCombResultNamesAttr,
                         builder.getArrayAttr(generatedNames));
    if (ownsWrapperResult && frozen.wrapperName)
      partition->setAttr("pyc.name", frozen.wrapperName);
    begin = end;
  }

  for (auto [oldResult, replacement] :
       llvm::zip(original.getResults(), replacements)) {
    if (!replacement) {
      original.emitOpError("failed to preserve an original comb result");
      return failure();
    }
    oldResult.replaceAllUsesWith(replacement);
  }
  original.erase();
  return success();
}

struct PartitionFusedCombPass
    : public PassWrapper<PartitionFusedCombPass, OperationPass<func::FuncOp>> {
  MLIR_DEFINE_EXPLICIT_INTERNAL_INLINE_TYPE_ID(PartitionFusedCombPass)

  Option<unsigned> maxNodes{
      *this, "max-nodes",
      llvm::cl::desc("Maximum operation count in each local fused-comb part"),
      llvm::cl::init(35)};

  PartitionFusedCombPass() = default;
  PartitionFusedCombPass(const PartitionFusedCombPass &other)
      : PassWrapper(other) {}
  explicit PartitionFusedCombPass(unsigned requestedMaxNodes) {
    maxNodes = requestedMaxNodes;
  }

  StringRef getArgument() const override { return "pyc-partition-fused-comb"; }
  StringRef getDescription() const override {
    return "Independently split top-level fused pyc.comb regions into local "
           "sibling DAG partitions";
  }

  void runOnOperation() override {
    if (maxNodes == 0)
      return;

    func::FuncOp function = getOperation();
    llvm::SmallVector<CombOp> candidates;
    function.walk([&](CombOp comb) {
      if (comb->getParentOfType<CombOp>() || hasPartitionMetadata(comb))
        return;
      candidates.push_back(comb);
    });

    llvm::SmallVector<FrozenCombPlan> plans;
    plans.reserve(candidates.size());
    uint64_t nextParentId = 0;
    for (CombOp comb : candidates) {
      auto graph = FusedCombDepGraph::build(comb);
      if (failed(graph)) {
        signalPassFailure();
        return;
      }
      if ((*graph)->getOperations().size() <= maxNodes)
        continue;
      auto partition = planFusedCombPartitions(**graph, maxNodes);
      if (failed(partition)) {
        comb.emitOpError("failed to construct a local partition plan");
        signalPassFailure();
        return;
      }
      llvm::SmallVector<StringAttr> resultNames;
      StringAttr wrapperName;
      if (failed(readObservableNames(comb, resultNames, wrapperName))) {
        signalPassFailure();
        return;
      }
      plans.push_back(
          FrozenCombPlan{comb, std::move(*graph), std::move(*partition),
                         std::move(resultNames), wrapperName, nextParentId++});
    }

    for (FrozenCombPlan &plan : plans) {
      if (failed(materialize(plan, maxNodes))) {
        signalPassFailure();
        return;
      }
    }
  }
};

} // namespace

std::unique_ptr<mlir::Pass> createPartitionFusedCombPass(unsigned maxNodes) {
  return std::make_unique<PartitionFusedCombPass>(maxNodes);
}

static PassRegistration<PartitionFusedCombPass> pass;

} // namespace pyc
