#include "pyc/Transforms/Passes.h"

#include "pyc/Dialect/PYC/PYCOps.h"

#include "mlir/Dialect/Func/IR/FuncOps.h"
#include "mlir/IR/Builders.h"
#include "mlir/Pass/Pass.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallVector.h"

using namespace mlir;

namespace pyc {
namespace {

// Generic DCE and CSE do not understand debug attributes. Give each explicitly
// retained pure value an observation alias before cleanup. Existing producer
// optimization barriers stay intact and every observation remains addressable.
// The dedicated resource has no hardware state or combinational-cut semantics.
struct PreserveObservationsPass
    : PassWrapper<PreserveObservationsPass, OperationPass<func::FuncOp>> {
  MLIR_DEFINE_EXPLICIT_INTERNAL_INLINE_TYPE_ID(PreserveObservationsPass)

  StringRef getArgument() const override { return "pyc-preserve-observations"; }
  StringRef getDescription() const override {
    return "Preserve explicitly retained debug values through generic cleanup";
  }

  void runOnOperation() override {
    SmallVector<Operation *> observations;
    getOperation().walk([&](Operation *op) {
      if (isDebugObservation(op))
        observations.push_back(op);
    });
    OpBuilder builder(getOperation().getContext());
    for (Operation *op : observations) {
      // Normalize the unit spelling for existing state/instance cleanup passes.
      op->setAttr("pyc.debug_keep", builder.getBoolAttr(true));
      if (isa<pyc::AliasOp>(op) || op->getNumResults() == 0 ||
          (!isHardwarePure(op) && !isa<pyc::WireOp>(op)))
        continue;
      Attribute name = op->getAttr("pyc.name");
      // Keep the producer's existing optimization barrier. On a repeated pass,
      // each result already has a retained alias and needs no second root.
      if (!name && llvm::all_of(op->getResults(), [](Value value) {
            return llvm::any_of(value.getUsers(), [](Operation *user) {
              return isa<pyc::AliasOp>(user) && isDebugObservation(user);
            });
          }))
        continue;
      op->removeAttr("pyc.name");
      builder.setInsertionPointAfter(op);
      for (Value value : op->getResults()) {
        auto alias = builder.create<pyc::AliasOp>(op->getLoc(), value.getType(), value);
        alias->setAttr("pyc.debug_keep", builder.getBoolAttr(true));
        if (name && op->getNumResults() == 1)
          alias->setAttr("pyc.name", name);
        value.replaceUsesWithIf(alias.getResult(), [&](OpOperand &use) {
          if (use.getOwner() == alias)
            return false;
          // Assign destinations must remain the original wire definition.
          return !isa<pyc::WireOp>(op) ||
                 !isa<pyc::AssignOp>(use.getOwner()) ||
                 use.getOperandNumber() != 0;
        });
      }
    }
  }
};

} // namespace

std::unique_ptr<Pass> createPreserveObservationsPass() {
  return std::make_unique<PreserveObservationsPass>();
}

static PassRegistration<PreserveObservationsPass> pass;
} // namespace pyc
