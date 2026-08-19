#include "pyc/Transforms/Passes.h"
#include "pyc/Transforms/StateOptimization.h"

#include "pyc/Dialect/PYC/PYCOps.h"

#include "mlir/Dialect/Func/IR/FuncOps.h"
#include "mlir/IR/Builders.h"
#include "mlir/Pass/Pass.h"
#include "llvm/ADT/SmallVector.h"

using namespace mlir;

namespace pyc {
namespace {

static bool isObservationIdentity(llvm::StringRef name) {
  return name == "pyc.name" || name == "pyc.debug_keep" ||
         name == "pyc.observable" || name.starts_with("pyc.probe") ||
         name.starts_with("pyc.trace");
}

static int64_t stripObservationIdentity(Operation *op) {
  llvm::SmallVector<StringAttr> names;
  for (NamedAttribute attr : op->getAttrs())
    if (isObservationIdentity(attr.getName().strref()))
      names.push_back(attr.getName());
  for (StringAttr name : names)
    op->removeAttr(name);
  return static_cast<int64_t>(names.size());
}

static bool aliasesState(pyc::AliasOp alias) {
  Value source = stripStateAliases(alias.getIn());
  return source.getDefiningOp<pyc::RegOp>() ||
         source.getDefiningOp<pyc::DelayLineOp>();
}

struct StripStateObservabilityPass
    : public PassWrapper<StripStateObservabilityPass,
                         OperationPass<func::FuncOp>> {
  MLIR_DEFINE_EXPLICIT_INTERNAL_INLINE_TYPE_ID(StripStateObservabilityPass)

  StringRef getArgument() const override {
    return "pyc-strip-state-observability";
  }
  StringRef getDescription() const override {
    return "Drop explicit state debug/probe/name identities for performance";
  }

  void runOnOperation() override {
    func::FuncOp function = getOperation();
    int64_t attrsStripped = 0;
    llvm::SmallVector<pyc::AliasOp> stateAliases;

    function.walk([&](Operation *op) {
      if (isa<pyc::RegOp, pyc::DelayLineOp>(op)) {
        attrsStripped += stripObservationIdentity(op);
        return;
      }
      if (auto alias = dyn_cast<pyc::AliasOp>(op); alias && aliasesState(alias)) {
        attrsStripped += stripObservationIdentity(op);
        stateAliases.push_back(alias);
      }
    });

    int64_t aliasesRemoved = 0;
    // SSA definitions precede their users, so reverse IR order removes an
    // unused alias chain from its leaves back to the state value in one pass.
    for (pyc::AliasOp alias : llvm::reverse(stateAliases)) {
      if (!alias.getResult().use_empty())
        continue;
      alias.erase();
      ++aliasesRemoved;
    }

    OpBuilder builder(function.getContext());
    function->setAttr("pyc.stats.state_opt_observability_attrs_stripped",
                      builder.getI64IntegerAttr(attrsStripped));
    function->setAttr("pyc.stats.state_opt_observation_aliases_removed",
                      builder.getI64IntegerAttr(aliasesRemoved));
  }
};

} // namespace

std::unique_ptr<::mlir::Pass> createStripStateObservabilityPass() {
  return std::make_unique<StripStateObservabilityPass>();
}

static PassRegistration<StripStateObservabilityPass> pass;

} // namespace pyc
