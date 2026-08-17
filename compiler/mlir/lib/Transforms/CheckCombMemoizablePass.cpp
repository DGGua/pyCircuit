#include "pyc/Transforms/CombMemoization.h"
#include "pyc/Transforms/Passes.h"

#include "pyc/Dialect/PYC/PYCOps.h"

#include "mlir/Dialect/Func/IR/FuncOps.h"
#include "mlir/Pass/Pass.h"
#include "llvm/ADT/STLExtras.h"

using namespace mlir;

namespace pyc {
namespace {

struct CheckCombMemoizablePass
    : public PassWrapper<CheckCombMemoizablePass, OperationPass<func::FuncOp>> {
  MLIR_DEFINE_EXPLICIT_INTERNAL_INLINE_TYPE_ID(CheckCombMemoizablePass)

  StringRef getArgument() const override { return "pyc-check-comb-memoizable"; }
  StringRef getDescription() const override {
    return "Verify that pyc.comb regions are deterministic and "
           "equality-comparable";
  }

  void runOnOperation() override {
    func::FuncOp function = getOperation();
    WalkResult result = function.walk([&](pyc::CombOp comb) {
      if (comb.getBody().empty() || !llvm::hasSingleElement(comb.getBody())) {
        comb.emitOpError(
            "must have one body block before memoization can be checked");
        return WalkResult::interrupt();
      }
      for (Type type : comb.getOperandTypes()) {
        if (!isMemoizableCombType(type)) {
          comb.emitOpError("memoized comb input has unsupported type ") << type;
          return WalkResult::interrupt();
        }
      }
      for (Type type : comb.getResultTypes()) {
        if (!isMemoizableCombType(type)) {
          comb.emitOpError("memoized comb output has unsupported type ")
              << type;
          return WalkResult::interrupt();
        }
      }

      Block &body = comb.getBody().front();
      for (Operation &op : body.without_terminator()) {
        if (!isMemoizableCombOperation(&op)) {
          op.emitOpError(
              "is not on the deterministic pyc.comb memoization whitelist");
          return WalkResult::interrupt();
        }
        for (Type type : op.getOperandTypes()) {
          if (!isMemoizableCombType(type)) {
            op.emitOpError("memoized operand has unsupported type ") << type;
            return WalkResult::interrupt();
          }
        }
        for (Type type : op.getResultTypes()) {
          if (!isMemoizableCombType(type)) {
            op.emitOpError("memoized result has unsupported type ") << type;
            return WalkResult::interrupt();
          }
        }
      }
      return WalkResult::advance();
    });
    if (result.wasInterrupted())
      signalPassFailure();
  }
};

} // namespace

std::unique_ptr<mlir::Pass> createCheckCombMemoizablePass() {
  return std::make_unique<CheckCombMemoizablePass>();
}

static PassRegistration<CheckCombMemoizablePass> pass;

} // namespace pyc
