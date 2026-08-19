#include "pyc/Transforms/Passes.h"

#include "pyc/Dialect/PYC/PYCOps.h"

#include "mlir/Dialect/Func/IR/FuncOps.h"
#include "mlir/IR/Builders.h"
#include "mlir/IR/IRMapping.h"
#include "mlir/Pass/Pass.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/STLExtras.h"

using namespace mlir;

namespace pyc {
namespace {

struct CombBoundaryPlan {
  llvm::SmallVector<unsigned> keptInputs;
  llvm::SmallVector<unsigned> outputCanonical;
  bool changed = false;
};

static bool hasObservableResultIdentity(pyc::CombOp comb) {
  // A named comb result is an externally visible identity.  Keep its result
  // arity stable even when two yielded SSA values happen to be equal.
  if (auto name = comb->getAttrOfType<StringAttr>("pyc.name"))
    if (!name.getValue().empty())
      return true;
  if (auto names = comb->getAttrOfType<ArrayAttr>("pyc.comb.result_names"))
    return llvm::any_of(names, [](Attribute attr) {
      auto name = dyn_cast<StringAttr>(attr);
      return name && !name.getValue().empty();
    });
  for (Operation &op : comb.getBody().front().without_terminator()) {
    if (auto name = op.getAttrOfType<StringAttr>("pyc.name"))
      if (!name.getValue().empty())
        return true;
    if (auto keep = op.getAttrOfType<BoolAttr>("pyc.debug_keep"))
      if (keep.getValue())
        return true;
  }
  return false;
}

static CombBoundaryPlan analyzeComb(pyc::CombOp comb) {
  CombBoundaryPlan plan;
  Block &body = comb.getBody().front();
  auto yield = cast<pyc::YieldOp>(body.getTerminator());

  llvm::DenseMap<Value, unsigned> firstInput;
  for (auto item : llvm::enumerate(comb.getInputs())) {
    unsigned index = item.index();
    Value input = item.value();
    BlockArgument argument = body.getArgument(index);
    auto found = firstInput.find(input);
    if (argument.use_empty()) {
      plan.changed = true;
      continue;
    }
    if (found != firstInput.end()) {
      // Duplicate external inputs need only one snapshot/parameter.  The
      // rebuild phase maps the duplicate block argument to the first one.
      plan.changed = true;
      continue;
    }
    firstInput[input] = plan.keptInputs.size();
    plan.keptInputs.push_back(index);
  }

  llvm::DenseMap<Value, unsigned> firstOutput;
  const bool preserveOutputArity = hasObservableResultIdentity(comb);
  for (auto [index, value] : llvm::enumerate(yield.getValues())) {
    unsigned canonical = index;
    if (!preserveOutputArity) {
      auto found = firstOutput.find(value);
      if (found != firstOutput.end()) {
        canonical = found->second;
        plan.changed = true;
      } else {
        firstOutput[value] = index;
      }
    }
    plan.outputCanonical.push_back(canonical);
  }
  return plan;
}

static LogicalResult rebuildComb(pyc::CombOp comb, const CombBoundaryPlan &plan) {
  Block &oldBody = comb.getBody().front();
  auto oldYield = cast<pyc::YieldOp>(oldBody.getTerminator());

  llvm::SmallVector<Value> newInputs;
  newInputs.reserve(plan.keptInputs.size());
  llvm::SmallVector<Type> newOutputTypes;
  llvm::SmallVector<unsigned> uniqueOutputs;
  for (unsigned index : plan.keptInputs)
    newInputs.push_back(comb.getInputs()[index]);
  for (auto [index, canonical] : llvm::enumerate(plan.outputCanonical)) {
    if (canonical != index)
      continue;
    uniqueOutputs.push_back(index);
    newOutputTypes.push_back(comb.getResult(index).getType());
  }

  OpBuilder builder(comb);
  auto replacement = builder.create<pyc::CombOp>(comb.getLoc(), newOutputTypes, newInputs);
  replacement->setAttrs(comb->getAttrs());
  Block *newBody = new Block();
  replacement.getBody().push_back(newBody);
  for (Value input : newInputs)
    newBody->addArgument(input.getType(), comb.getLoc());

  IRMapping mapping;
  for (auto [newIndex, oldIndex] : llvm::enumerate(plan.keptInputs))
    mapping.map(oldBody.getArgument(oldIndex), newBody->getArgument(newIndex));
  for (auto item : llvm::enumerate(comb.getInputs())) {
    unsigned index = item.index();
    Value input = item.value();
    auto found = llvm::find(plan.keptInputs, index);
    if (found != plan.keptInputs.end())
      continue;
    auto first = llvm::find_if(plan.keptInputs, [&](unsigned kept) {
      return comb.getInputs()[kept] == input;
    });
    if (first != plan.keptInputs.end())
      mapping.map(oldBody.getArgument(index),
                  newBody->getArgument(std::distance(plan.keptInputs.begin(), first)));
  }

  builder.setInsertionPointToStart(newBody);
  for (Operation &op : oldBody.without_terminator()) {
    Operation *clone = builder.clone(op, mapping);
    for (auto [oldResult, newResult] : llvm::zip(op.getResults(), clone->getResults()))
      mapping.map(oldResult, newResult);
  }

  llvm::SmallVector<Value> newYieldValues;
  newYieldValues.reserve(uniqueOutputs.size());
  for (unsigned oldIndex : uniqueOutputs) {
    Value mapped = mapping.lookupOrNull(oldYield.getOperand(oldIndex));
    if (!mapped)
      return comb.emitError("host boundary rebuild failed to map comb output");
    newYieldValues.push_back(mapped);
  }
  builder.create<pyc::YieldOp>(comb.getLoc(), newYieldValues);

  llvm::DenseMap<unsigned, unsigned> canonicalToNew;
  for (auto [newIndex, oldIndex] : llvm::enumerate(uniqueOutputs))
    canonicalToNew[oldIndex] = newIndex;
  for (auto [oldIndex, canonical] : llvm::enumerate(plan.outputCanonical))
    comb.getResult(oldIndex).replaceAllUsesWith(
        replacement.getResult(canonicalToNew.lookup(canonical)));
  comb.erase();
  return success();
}

static bool isTransparentSingleResultComb(pyc::CombOp comb, Value &input) {
  if (comb.getInputs().size() != 1 || comb.getNumResults() != 1 || !comb->getAttrs().empty())
    return false;
  Block &body = comb.getBody().front();
  for (Operation &op : body.without_terminator()) {
    if (auto name = op.getAttrOfType<StringAttr>("pyc.name"))
      if (!name.getValue().empty())
        return false;
    if (auto keep = op.getAttrOfType<BoolAttr>("pyc.debug_keep"))
      if (keep.getValue())
        return false;
  }
  auto yield = dyn_cast<pyc::YieldOp>(body.getTerminator());
  if (!yield || yield.getNumOperands() != 1)
    return false;
  Value value = yield.getOperand(0);
  while (auto alias = value.getDefiningOp<pyc::AliasOp>())
    value = alias.getIn();
  if (value != body.getArgument(0))
    return false;
  input = comb.getInputs()[0];
  return true;
}

struct CombHostBoundaryPass
    : public PassWrapper<CombHostBoundaryPass, OperationPass<func::FuncOp>> {
  MLIR_DEFINE_EXPLICIT_INTERNAL_INLINE_TYPE_ID(CombHostBoundaryPass)

  StringRef getArgument() const override { return "pyc-comb-host-boundary"; }
  StringRef getDescription() const override {
    return "Reduce redundant pyc.comb inputs, outputs, and transparent wrappers";
  }

  void runOnOperation() override {
    func::FuncOp function = getOperation();
    if (auto structural = function->getAttrOfType<StringAttr>("pyc.emit.structural")) {
      StringRef value = structural.getValue();
      if (value.equals_insensitive("true") || value == "1")
        return;
    }
    llvm::SmallVector<pyc::CombOp> combs;
    function.walk([&](pyc::CombOp comb) { combs.push_back(comb); });

    for (pyc::CombOp comb : combs) {
      if (comb->use_empty())
        continue;
      if (comb.getBody().empty() || !llvm::hasSingleElement(comb.getBody()))
        continue;
      auto yield = dyn_cast<pyc::YieldOp>(comb.getBody().front().getTerminator());
      if (!yield)
        continue;
      CombBoundaryPlan plan = analyzeComb(comb);
      if (plan.changed && failed(rebuildComb(comb, plan))) {
        signalPassFailure();
        return;
      }
    }

    // A second sweep removes wrappers exposed by input/output normalization.
    combs.clear();
    function.walk([&](pyc::CombOp comb) { combs.push_back(comb); });
    for (pyc::CombOp comb : combs) {
      Value input;
      if (!isTransparentSingleResultComb(comb, input))
        continue;
      comb.getResult(0).replaceAllUsesWith(input);
      comb.erase();
    }
  }
};

} // namespace

std::unique_ptr<::mlir::Pass> createCombHostBoundaryPass() {
  return std::make_unique<CombHostBoundaryPass>();
}

static PassRegistration<CombHostBoundaryPass> pass;

} // namespace pyc
