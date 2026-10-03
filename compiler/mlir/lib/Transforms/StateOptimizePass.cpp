#include "pyc/Transforms/Passes.h"
#include "pyc/Transforms/StateOptimization.h"

#include "mlir/Dialect/Arith/IR/Arith.h"
#include "mlir/Dialect/Func/IR/FuncOps.h"
#include "mlir/IR/PatternMatch.h"
#include "mlir/IR/BuiltinAttributes.h"
#include "mlir/Pass/Pass.h"
#include "mlir/Transforms/GreedyPatternRewriteDriver.h"
#include "mlir/Transforms/CSE.h"
#include "mlir/IR/Dominance.h"
#include "llvm/ADT/STLExtras.h"

using namespace mlir;

namespace pyc {
namespace {

static void setI64Attr(Operation *op, StringRef name, int64_t value) {
  OpBuilder builder(op->getContext());
  op->setAttr(name, builder.getI64IntegerAttr(value));
}

static int64_t getI64Attr(Operation *op, StringRef name, int64_t fallback) {
  if (auto value = op->getAttrOfType<IntegerAttr>(name))
    return value.getInt();
  return fallback;
}

// In-pass replacement for the upstream canonicalize+CSE pass pair pycc used
// between merge rounds: greedy folding absorbs every op's canonicalization
// patterns and folders, then explicit CSE dedupes the exposed equivalents.
static LogicalResult canonicalizeAndCSE(func::FuncOp function) {
  if (failed(applyPatternsAndFoldGreedily(function, RewritePatternSet(
                                                    function.getContext()),
                                          GreedyRewriteConfig())))
    return failure();
  RewriterBase::Listener listener;
  IRRewriter rewriter(function.getContext(), &listener);
  DominanceInfo domInfo(function);
  bool changed = false;
  eliminateCommonSubExpressions(rewriter, domInfo, function, &changed);
  return success();
}

struct StateOptimizePass
    : public PassWrapper<StateOptimizePass, OperationPass<func::FuncOp>> {
  MLIR_DEFINE_EXPLICIT_INTERNAL_INLINE_TYPE_ID(StateOptimizePass)

  StateOptimizePass() = default;
  StateOptimizePass(const StateOptimizePass &other)
      : PassWrapper(other), tunables(other.tunables) {}
  explicit StateOptimizePass(const StateOptimizeTunables &tunables)
      : tunables(tunables) {}

  StringRef getArgument() const override { return "pyc-state-optimize"; }
  StringRef getDescription() const override {
    return "Unified structural state optimization: merge, retime, chain, "
           "and pack state with observation identities preserved";
  }

  void runOnOperation() override {
    func::FuncOp function = getOperation();

    // Round 1: low-risk equivalent-state merge.
    const int64_t mergeRoundsBefore =
        getI64Attr(function, "pyc.stats.state_opt_merge_rounds", 0);
    runDelayChainMergeRound(function, /*cascadeRound=*/false,
                            /*accumulateStats=*/false);
    setI64Attr(function, "pyc.stats.state_opt_merge_rounds",
               mergeRoundsBefore + 1);

    // The first state merge can expose equivalent combinational cones.
    // Canonicalize/CSE once, then run one bounded merge refinement.
    if (failed(canonicalizeAndCSE(function))) {
      signalPassFailure();
      return;
    }
    runDelayChainMergeRound(function, /*cascadeRound=*/true,
                            /*accumulateStats=*/true);
    setI64Attr(function, "pyc.stats.state_opt_merge_rounds",
               mergeRoundsBefore + 2);

    // Retime after the two low-risk merge-only rounds: a local retime must
    // not consume a state that has a more profitable global equivalent-state
    // or direct-chain rewrite.
    runRetimePipelines(function, tunables.retimeMaxStages,
                       tunables.retimeMaxExtraCombOps,
                       tunables.retimeMaxCombDepth,
                       /*accumulateStats=*/false);
    // Sinking moves consumer cones onto source.next, placing them in the
    // same combinational cone as the producers of next. The fusion win
    // (shared subexpressions, one scheduled comb region) only materializes
    // once CSE deduplicates the merged cone — run it here so chain
    // forming, history sharing, and packing see the deduplicated values
    // instead of structurally-identical duplicates.
    if (failed(canonicalizeAndCSE(function))) {
      signalPassFailure();
      return;
    }
    runEliminateDeadState(function);
    // Form ordinary direct chains only after retiming selected the more
    // general computed pipelines; also share equivalent histories here.
    runDelayChainFormAndShare(function, /*accumulateStats=*/true);
    // Packing works on the final state set.
    runPackStateLanes(function, tunables.packMaxWidth);
  }

  StateOptimizeTunables tunables;
};

} // namespace

std::unique_ptr<::mlir::Pass>
createStateOptimizePass(const StateOptimizeTunables &tunables) {
  return std::make_unique<StateOptimizePass>(tunables);
}

static PassRegistration<StateOptimizePass> pass;

} // namespace pyc
