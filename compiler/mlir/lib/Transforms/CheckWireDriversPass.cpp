#include "pyc/Transforms/Passes.h"

#include "pyc/Dialect/PYC/PYCOps.h"

#include "mlir/Dialect/Func/IR/FuncOps.h"
#include "mlir/IR/BuiltinAttributes.h"
#include "mlir/Pass/Pass.h"
#include "llvm/ADT/SmallVector.h"

using namespace mlir;

namespace pyc {
namespace {

/// True when a use reads the wire value rather than declaring one of its
/// drivers.  AssignOp's operand #0 is the destination; every other use,
/// including using a wire as another AssignOp's source, observes the value.
static bool isWireRead(OpOperand &use) {
  auto assign = dyn_cast<pyc::AssignOp>(use.getOwner());
  return !assign || use.getOperandNumber() != 0;
}

static bool isExplicitlyObservable(pyc::WireOp wire) {
  if (auto name = wire->getAttrOfType<StringAttr>("pyc.name"))
    if (!name.getValue().empty())
      return true;
  if (auto keep = wire->getAttrOfType<BoolAttr>("pyc.debug_keep"))
    return keep.getValue();
  return false;
}

struct CheckWireDriversPass
    : public PassWrapper<CheckWireDriversPass,
                         OperationPass<func::FuncOp>> {
  MLIR_DEFINE_EXPLICIT_INTERNAL_INLINE_TYPE_ID(CheckWireDriversPass)

  StringRef getArgument() const override { return "pyc-check-wire-drivers"; }
  StringRef getDescription() const override {
    return "Verify single-driver pyc.wire plumbing before elimination";
  }

  void runOnOperation() override {
    func::FuncOp function = getOperation();
    bool failedAny = false;

    function.walk([&](pyc::WireOp wire) {
      llvm::SmallVector<pyc::AssignOp> drivers;
      bool isRead = false;
      for (OpOperand &use : wire.getResult().getUses()) {
        auto assign = dyn_cast<pyc::AssignOp>(use.getOwner());
        if (assign && use.getOperandNumber() == 0) {
          drivers.push_back(assign);
          continue;
        }
        isRead |= isWireRead(use);
      }

      if (drivers.size() > 1) {
        wire.emitOpError("has multiple pyc.assign drivers; pyc.wire is "
                         "single-driver plumbing (expected exactly one, got ")
            << drivers.size() << ")";
        failedAny = true;
        return;
      }

      if (drivers.empty() && (isRead || isExplicitlyObservable(wire))) {
        wire.emitOpError("has no pyc.assign driver but is observable or read; "
                         "expected exactly one driver");
        failedAny = true;
      }
    });

    if (failedAny)
      signalPassFailure();
  }
};

} // namespace

std::unique_ptr<::mlir::Pass> createCheckWireDriversPass() {
  return std::make_unique<CheckWireDriversPass>();
}

static PassRegistration<CheckWireDriversPass> pass;

} // namespace pyc
