#include "pyc/Transforms/Passes.h"
#include "pyc/Transforms/StateOptimization.h"

#include "mlir/Dialect/Func/IR/FuncOps.h"
#include "mlir/IR/Builders.h"
#include "mlir/Pass/Pass.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/SmallVector.h"

#include <tuple>

using namespace mlir;

namespace pyc {
namespace {

struct CandidateStats {
  int64_t regsSeen = 0;
  int64_t generatedRegs = 0;
  int64_t pinnedRegs = 0;
  int64_t mergeCandidates = 0;
  int64_t generatedChains = 0;
  int64_t generatedChainRegs = 0;
  int64_t structuralChains = 0;
  int64_t structuralChainRegs = 0;
  int64_t structuralOnlyChains = 0;
  int64_t structuralOnlyChainRegs = 0;
};

static void setI64Attr(Operation *op, llvm::StringRef name, int64_t value) {
  OpBuilder builder(op->getContext());
  op->setAttr(name, builder.getI64IntegerAttr(value));
}

static std::pair<int64_t, int64_t>
countChains(ArrayRef<pyc::RegOp> regs, DelayChainMode mode,
            const StateObservabilityAnalysis &observability,
            int64_t *onlyChains = nullptr, int64_t *onlyRegs = nullptr) {
  llvm::DenseSet<Operation *> claimed;
  int64_t chains = 0;
  int64_t chainRegs = 0;
  for (pyc::RegOp tail : llvm::reverse(regs)) {
    if (claimed.contains(tail.getOperation()) ||
        !isStateOptimizationCandidate(tail, mode, observability))
      continue;

    llvm::SmallVector<pyc::RegOp> chain{tail};
    pyc::RegOp cursor = tail;
    while (auto link = matchStateChainPredecessor(
               cursor, tail, mode, observability)) {
      if (claimed.contains(link->predecessor.getOperation()))
        break;
      cursor = link->predecessor;
      chain.push_back(cursor);
    }
    if (chain.size() < 2)
      continue;

    ++chains;
    chainRegs += static_cast<int64_t>(chain.size());
    bool allGenerated = true;
    for (pyc::RegOp reg : chain) {
      claimed.insert(reg.getOperation());
      allGenerated &= isCycleBalanceGenerated(reg);
    }
    if (!allGenerated && onlyChains && onlyRegs) {
      ++*onlyChains;
      *onlyRegs += static_cast<int64_t>(chain.size());
    }
  }
  return {chains, chainRegs};
}

struct AnalyzeStateOptimizationPass
    : public PassWrapper<AnalyzeStateOptimizationPass,
                         OperationPass<func::FuncOp>> {
  MLIR_DEFINE_EXPLICIT_INTERNAL_INLINE_TYPE_ID(AnalyzeStateOptimizationPass)

  StringRef getArgument() const override {
    return "pyc-analyze-state-optimization";
  }
  StringRef getDescription() const override {
    return "Report equivalent-state and structural delay-chain candidates without rewriting IR";
  }

  void runOnOperation() override {
    func::FuncOp function = getOperation();
    StateObservabilityAnalysis observability(function);
    CandidateStats stats;

    llvm::SmallVector<pyc::RegOp> regs;
    function.walk([&](pyc::RegOp reg) {
      regs.push_back(reg);
      ++stats.regsSeen;
      stats.generatedRegs += isCycleBalanceGenerated(reg) ? 1 : 0;
      stats.pinnedRegs += observability.isPinned(reg.getOperation()) ? 1 : 0;
    });

    llvm::DenseMap<std::size_t, llvm::SmallVector<pyc::RegOp>> mergeBuckets;
    for (pyc::RegOp reg : regs) {
      if (!isStateOptimizationCandidate(reg, DelayChainMode::Structural,
                                        observability))
        continue;
      auto &bucket = mergeBuckets[registerStateHash(reg)];
      bool duplicate = false;
      for (pyc::RegOp existing : bucket) {
        if (equivalentRegisterState(existing, reg)) {
          duplicate = true;
          break;
        }
      }
      if (duplicate)
        ++stats.mergeCandidates;
      else
        bucket.push_back(reg);
    }

    std::tie(stats.generatedChains, stats.generatedChainRegs) =
        countChains(regs, DelayChainMode::Generated, observability);
    std::tie(stats.structuralChains, stats.structuralChainRegs) = countChains(
        regs, DelayChainMode::Structural, observability,
        &stats.structuralOnlyChains, &stats.structuralOnlyChainRegs);

    setI64Attr(function, "pyc.stats.state_opt_regs_seen", stats.regsSeen);
    setI64Attr(function, "pyc.stats.state_opt_generated_regs",
               stats.generatedRegs);
    setI64Attr(function, "pyc.stats.state_opt_pinned_regs", stats.pinnedRegs);
    setI64Attr(function, "pyc.stats.state_opt_merge_candidates",
               stats.mergeCandidates);
    setI64Attr(function, "pyc.stats.state_opt_generated_chains",
               stats.generatedChains);
    setI64Attr(function, "pyc.stats.state_opt_generated_chain_regs",
               stats.generatedChainRegs);
    setI64Attr(function, "pyc.stats.state_opt_structural_chains",
               stats.structuralChains);
    setI64Attr(function, "pyc.stats.state_opt_structural_chain_regs",
               stats.structuralChainRegs);
    setI64Attr(function, "pyc.stats.state_opt_structural_only_chains",
               stats.structuralOnlyChains);
    setI64Attr(function, "pyc.stats.state_opt_structural_only_chain_regs",
               stats.structuralOnlyChainRegs);
  }
};

} // namespace

std::unique_ptr<::mlir::Pass> createAnalyzeStateOptimizationPass() {
  return std::make_unique<AnalyzeStateOptimizationPass>();
}

static PassRegistration<AnalyzeStateOptimizationPass> pass;

} // namespace pyc
