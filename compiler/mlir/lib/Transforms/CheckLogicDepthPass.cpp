#include "pyc/Transforms/CombDepGraph.h"
#include "pyc/Transforms/Passes.h"

#include "pyc/Dialect/PYC/PYCOps.h"

#include "mlir/Dialect/Func/IR/FuncOps.h"
#include "mlir/IR/BuiltinAttributes.h"
#include "mlir/IR/BuiltinOps.h"
#include "mlir/Pass/Pass.h"

#include <algorithm>
#include <cstdint>
#include <limits>

using namespace mlir;

namespace pyc {
namespace {

static bool isSequentialEndpoint(Operation *op) {
  return isa<pyc::RegOp, pyc::DelayLineOp, pyc::FifoOp, pyc::ByteMemOp,
             pyc::SyncMemOp, pyc::SyncMemDPOp, pyc::AsyncFifoOp,
             pyc::CdcSyncOp>(op);
}

/// Logic-depth checking is intentionally a consumer of FunctionCombDepGraph.
/// The graph builder owns the operation/primitive/comb/instance transfer
/// semantics and annotates every canonical value node with its maximum depth;
/// this pass only selects observable endpoints and checks their slack.
class CheckLogicDepthPass
    : public PassWrapper<CheckLogicDepthPass, OperationPass<ModuleOp>> {
public:
  MLIR_DEFINE_EXPLICIT_INTERNAL_INLINE_TYPE_ID(CheckLogicDepthPass)

  explicit CheckLogicDepthPass(unsigned depth = 32) : maxDepthLimit(depth) {}

  StringRef getArgument() const override { return "pyc-check-logic-depth"; }
  StringRef getDescription() const override {
    return "Check endpoint depth using the canonical same-TICK CombDepGraph";
  }

  void runOnOperation() override {
    ModuleOp module = getOperation();
    CombDepGraphCache graphCache(module);
    bool failedAny = false;

    for (func::FuncOp function : module.getOps<func::FuncOp>()) {
      if (function.isDeclaration() || function.getBody().empty())
        continue;

      const int64_t limit = static_cast<int64_t>(maxDepthLimit);
      auto graph = FunctionCombDepGraph::build(function, graphCache);
      if (failed(graph)) {
        function.emitOpError(
            "failed to build canonical graph for logic-depth checking");
        failedAny = true;
        continue;
      }
      if (failed((*graph)->stableTopologicalOrder())) {
        function.emitOpError(
            "cannot compute logic depth for a cyclic CombDepGraph");
        failedAny = true;
        continue;
      }

      int64_t maxDepth = 0;
      int64_t wns = std::numeric_limits<int64_t>::max();
      int64_t tns = 0;
      bool failedThisFunction = false;

      auto observeEndpoint = [&](Operation *endpoint, Value value) {
        const CombDepValueNode *node = (*graph)->lookup(value);
        if (!node) {
          endpoint->emitError(
              "endpoint value is missing from the canonical CombDepGraph");
          failedThisFunction = true;
          return;
        }
        int64_t depth = node->logicDepth;
        maxDepth = std::max(maxDepth, depth);
        int64_t slack = limit - depth;
        wns = std::min(wns, slack);
        if (slack < 0)
          tns += slack;
        if (depth > limit) {
          endpoint->emitError("logic depth exceeds limit: depth=")
              << depth << " limit=" << limit;
          failedThisFunction = true;
        }
      };

      function.walk([&](Operation *op) {
        if (auto ret = dyn_cast<func::ReturnOp>(op)) {
          for (Value value : ret.getOperands())
            observeEndpoint(op, value);
          return;
        }
        if (auto assertion = dyn_cast<pyc::AssertOp>(op)) {
          observeEndpoint(op, assertion.getCond());
          return;
        }
        if (isSequentialEndpoint(op))
          for (Value value : op->getOperands())
            observeEndpoint(op, value);
      });

      if (wns == std::numeric_limits<int64_t>::max())
        wns = limit;
      Type i64 = IntegerType::get(function.getContext(), 64);
      function->setAttr("pyc.logic_depth.max",
                        IntegerAttr::get(i64, maxDepth));
      function->setAttr("pyc.logic_depth.wns", IntegerAttr::get(i64, wns));
      function->setAttr("pyc.logic_depth.tns", IntegerAttr::get(i64, tns));

      failedAny |= failedThisFunction;
    }

    if (failedAny)
      signalPassFailure();
  }

private:
  unsigned maxDepthLimit = 32;
};

} // namespace

std::unique_ptr<::mlir::Pass> createCheckLogicDepthPass(unsigned logicDepth) {
  return std::make_unique<CheckLogicDepthPass>(logicDepth);
}

static PassRegistration<CheckLogicDepthPass> pass;

} // namespace pyc
