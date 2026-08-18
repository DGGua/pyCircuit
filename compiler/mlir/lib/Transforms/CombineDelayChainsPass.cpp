// Form compact delay-line state from proven serial register chains. The
// generated mode preserves the original cycle-balance-only policy; structural
// mode additionally merges equivalent unobservable state and admits untagged
// chains after the same sequential and observability proof.

#include "pyc/Transforms/Passes.h"
#include "pyc/Transforms/StateOptimization.h"

#include "pyc/Dialect/PYC/PYCOps.h"

#include "mlir/Dialect/Func/IR/FuncOps.h"
#include "mlir/IR/Builders.h"
#include "mlir/IR/BuiltinTypes.h"
#include "mlir/Pass/Pass.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallVector.h"

#include <cstddef>

using namespace mlir;

namespace pyc {
namespace {

constexpr llvm::StringLiteral kOptimizedByAttr = "pyc.optimized_by";
constexpr llvm::StringLiteral kCombineDelayChains = "combine_delay_chains";
constexpr llvm::StringLiteral kStructuralCombineDelayChains =
    "combine_delay_chains_structural";
constexpr llvm::StringLiteral kSourceRegCountAttr = "pyc.source_reg_count";
constexpr llvm::StringLiteral kSharedChainCountAttr = "pyc.shared_chain_count";
constexpr llvm::StringLiteral kStateMergedCountAttr = "pyc.state_merged_count";

struct CombineStats {
  int64_t chainsCombined = 0;
  int64_t regsCombined = 0;
  int64_t aliasesRemoved = 0;
  int64_t delayLinesCreated = 0;
  int64_t delayLinesMerged = 0;
  int64_t stateRegsMerged = 0;
  int64_t stateRegBitsRemoved = 0;
  int64_t structuralChainsCombined = 0;
  int64_t structuralRegsCombined = 0;
};

static int64_t getI64Attr(Operation *op, llvm::StringRef name,
                          int64_t fallback) {
  if (auto value = op->getAttrOfType<IntegerAttr>(name))
    return value.getInt();
  return fallback;
}

static void setI64Attr(Operation *op, llvm::StringRef name, int64_t value) {
  OpBuilder builder(op->getContext());
  op->setAttr(name, builder.getI64IntegerAttr(value));
}

static int64_t stateBitWidth(Type type) {
  if (auto integer = dyn_cast<IntegerType>(type))
    return static_cast<int64_t>(integer.getWidth());
  if (auto vector = dyn_cast<VectorType>(type)) {
    int64_t lanes = 1;
    for (int64_t dimension : vector.getShape())
      lanes *= dimension;
    if (auto element = dyn_cast<IntegerType>(vector.getElementType()))
      return lanes * static_cast<int64_t>(element.getWidth());
  }
  return 0;
}

static Value stripChainAliases(Value value, DelayChainMode mode) {
  while (auto alias = value.getDefiningOp<pyc::AliasOp>()) {
    if (!isTransparentChainAlias(alias, mode))
      break;
    value = alias.getIn();
  }
  return value;
}

static bool isDelayCandidate(
    pyc::DelayLineOp delay, DelayChainMode mode,
    const StateObservabilityAnalysis &observability) {
  if (!delay || shouldKeepStateOptimization(delay) ||
      hasStableStateName(delay) || observability.isPinned(delay.getOperation()))
    return false;
  if (mode == DelayChainMode::Generated)
    return isCycleBalanceGenerated(delay);
  return true;
}

static void mergeEquivalentStates(
    func::FuncOp function, CombineStats &stats,
    const StateObservabilityAnalysis &observability) {
  llvm::SmallVector<pyc::RegOp> regs;
  function.walk([&](pyc::RegOp reg) { regs.push_back(reg); });

  llvm::DenseMap<std::size_t, llvm::SmallVector<pyc::RegOp>> buckets;
  for (pyc::RegOp reg : regs) {
    if (!isStateOptimizationCandidate(reg, DelayChainMode::Structural,
                                      observability))
      continue;

    auto &bucket = buckets[registerStateHash(reg)];
    pyc::RegOp survivor;
    for (pyc::RegOp existing : bucket) {
      if (equivalentRegisterState(existing, reg)) {
        survivor = existing;
        break;
      }
    }
    if (!survivor) {
      bucket.push_back(reg);
      continue;
    }

    reg.getQ().replaceAllUsesWith(survivor.getQ());
    const int64_t survivorCount =
        getI64Attr(survivor, kStateMergedCountAttr, 1);
    setI64Attr(survivor, kStateMergedCountAttr, survivorCount + 1);
    survivor->setAttr(kOptimizedByAttr,
                      StringAttr::get(function.getContext(),
                                      "merge_equivalent_state"));
    ++stats.stateRegsMerged;
    stats.stateRegBitsRemoved += stateBitWidth(reg.getQ().getType());
    reg.erase();
  }
}

static void combineStateChains(
    func::FuncOp function, CombineStats &stats, DelayChainMode mode,
    const StateObservabilityAnalysis &observability) {
  llvm::SmallVector<pyc::RegOp> regs;
  function.walk([&](pyc::RegOp reg) {
    if (isStateOptimizationCandidate(reg, mode, observability))
      regs.push_back(reg);
  });

  llvm::DenseSet<Operation *> erased;
  for (pyc::RegOp tail : llvm::reverse(regs)) {
    if (erased.contains(tail.getOperation()))
      continue;

    llvm::SmallVector<pyc::RegOp> chainFromTail{tail};
    llvm::SmallVector<StateChainLink> linksFromTail;
    pyc::RegOp cursor = tail;
    while (auto link = matchStateChainPredecessor(
               cursor, tail, mode, observability)) {
      if (erased.contains(link->predecessor.getOperation()))
        break;
      linksFromTail.push_back(*link);
      cursor = link->predecessor;
      chainFromTail.push_back(cursor);
    }
    if (chainFromTail.size() < 2)
      continue;

    const int64_t depth = static_cast<int64_t>(chainFromTail.size());
    pyc::RegOp head = chainFromTail.back();
    const bool allGenerated = llvm::all_of(
        chainFromTail,
        [](pyc::RegOp reg) { return isCycleBalanceGenerated(reg); });

    OpBuilder builder(tail);
    auto delay = builder.create<pyc::DelayLineOp>(
        tail.getLoc(), tail.getQ().getType(), head.getClk(), head.getRst(),
        head.getEn(), stripChainAliases(head.getNext(), mode), head.getInit());
    delay->setAttr("depth", builder.getI64IntegerAttr(depth));
    if (allGenerated)
      delay->setAttr("pyc.generated",
                     builder.getStringAttr("cycle_balance"));
    const bool legacyGeneratedRewrite =
        allGenerated && mode == DelayChainMode::Generated;
    delay->setAttr(kOptimizedByAttr,
                   builder.getStringAttr(legacyGeneratedRewrite
                                             ? kCombineDelayChains
                                             : kStructuralCombineDelayChains));
    delay->setAttr(kSourceRegCountAttr,
                   builder.getI64IntegerAttr(depth));
    delay->setAttr(kSharedChainCountAttr, builder.getI64IntegerAttr(1));

    ++stats.chainsCombined;
    stats.regsCombined += depth;
    ++stats.delayLinesCreated;
    for (const StateChainLink &link : linksFromTail)
      stats.aliasesRemoved +=
          static_cast<int64_t>(link.aliasesFromConsumerToProducer.size());
    if (mode == DelayChainMode::Structural) {
      ++stats.structuralChainsCombined;
      stats.structuralRegsCombined += depth;
    }

    tail.getQ().replaceAllUsesWith(delay.getQ());

    for (unsigned i = 0; i < linksFromTail.size(); ++i) {
      pyc::RegOp reg = chainFromTail[i];
      erased.insert(reg.getOperation());
      reg.erase();
      for (pyc::AliasOp alias :
           linksFromTail[i].aliasesFromConsumerToProducer)
        alias.erase();
    }
    erased.insert(head.getOperation());
    head.erase();
  }
}

static void shareEquivalentDelayLines(
    func::FuncOp function, CombineStats &stats, DelayChainMode mode,
    const StateObservabilityAnalysis &observability) {
  llvm::SmallVector<pyc::DelayLineOp> delays;
  function.walk([&](pyc::DelayLineOp delay) {
    if (isDelayCandidate(delay, mode, observability))
      delays.push_back(delay);
  });

  llvm::DenseMap<std::size_t, llvm::SmallVector<pyc::DelayLineOp>> buckets;
  for (pyc::DelayLineOp delay : delays) {
    auto &bucket = buckets[delayLineStateHash(delay)];
    pyc::DelayLineOp survivor;
    for (pyc::DelayLineOp existing : bucket) {
      if (equivalentDelayLineState(existing, delay)) {
        survivor = existing;
        break;
      }
    }
    if (!survivor) {
      bucket.push_back(delay);
      continue;
    }

    const int64_t lhsRegs =
        getI64Attr(survivor, kSourceRegCountAttr, 0);
    const int64_t rhsRegs = getI64Attr(delay, kSourceRegCountAttr, 0);
    const int64_t lhsChains =
        getI64Attr(survivor, kSharedChainCountAttr, 0);
    const int64_t rhsChains =
        getI64Attr(delay, kSharedChainCountAttr, 0);
    if (lhsRegs || rhsRegs)
      setI64Attr(survivor, kSourceRegCountAttr, lhsRegs + rhsRegs);
    if (lhsChains || rhsChains)
      setI64Attr(survivor, kSharedChainCountAttr, lhsChains + rhsChains);

    delay.getQ().replaceAllUsesWith(survivor.getQ());
    delay.erase();
    ++stats.delayLinesMerged;
  }
}

static void writeCombineStats(func::FuncOp function,
                              const CombineStats &stats) {
  const int64_t stateOpsAfter =
      stats.delayLinesCreated > stats.delayLinesMerged
          ? stats.delayLinesCreated - stats.delayLinesMerged
          : 0;
  setI64Attr(function, "pyc.stats.delay_chains_combined",
             stats.chainsCombined);
  setI64Attr(function, "pyc.stats.delay_chain_regs_combined",
             stats.regsCombined);
  setI64Attr(function, "pyc.stats.delay_chain_aliases_removed",
             stats.aliasesRemoved);
  setI64Attr(function, "pyc.stats.delay_chain_delay_lines_created",
             stats.delayLinesCreated);
  setI64Attr(function, "pyc.stats.delay_chain_delay_lines_merged",
             stats.delayLinesMerged);
  setI64Attr(function, "pyc.stats.delay_chain_state_reads_before",
             stats.regsCombined);
  setI64Attr(function, "pyc.stats.delay_chain_state_reads_after",
             stateOpsAfter);
  setI64Attr(function, "pyc.stats.delay_chain_state_writes_before",
             stats.regsCombined);
  setI64Attr(function, "pyc.stats.delay_chain_state_writes_after",
             stateOpsAfter);
  setI64Attr(function, "pyc.stats.state_opt_regs_merged",
             stats.stateRegsMerged);
  setI64Attr(function, "pyc.stats.state_opt_reg_bits_removed",
             stats.stateRegBitsRemoved);
  setI64Attr(function, "pyc.stats.state_opt_structural_chains_combined",
             stats.structuralChainsCombined);
  setI64Attr(function, "pyc.stats.state_opt_structural_chain_regs_combined",
             stats.structuralRegsCombined);
}

struct CombineDelayChainsPass
    : public PassWrapper<CombineDelayChainsPass,
                         OperationPass<func::FuncOp>> {
  MLIR_DEFINE_EXPLICIT_INTERNAL_INLINE_TYPE_ID(CombineDelayChainsPass)

  CombineDelayChainsPass() = default;
  CombineDelayChainsPass(const CombineDelayChainsPass &other)
      : PassWrapper(other) {}
  explicit CombineDelayChainsPass(DelayChainMode mode) {
    modeOption = stringifyDelayChainMode(mode).str();
  }

  StringRef getArgument() const override { return "pyc-combine-delay-chains"; }
  StringRef getDescription() const override {
    return "Merge equivalent state and form proven fixed-depth delay lines";
  }

  Option<std::string> modeOption{
      *this, "mode",
      llvm::cl::desc("Candidate policy: generated|structural"),
      llvm::cl::init("generated")};

  void runOnOperation() override {
    auto mode = parseDelayChainMode(modeOption);
    if (!mode) {
      getOperation().emitError()
          << "invalid delay-chain mode '" << modeOption
          << "' (expected generated|structural)";
      signalPassFailure();
      return;
    }

    func::FuncOp function = getOperation();
    StateObservabilityAnalysis observability(function);
    CombineStats stats;
    if (*mode == DelayChainMode::Structural)
      mergeEquivalentStates(function, stats, observability);
    combineStateChains(function, stats, *mode, observability);
    shareEquivalentDelayLines(function, stats, *mode, observability);
    writeCombineStats(function, stats);
  }
};

} // namespace

std::unique_ptr<::mlir::Pass>
createCombineDelayChainsPass(DelayChainMode mode) {
  return std::make_unique<CombineDelayChainsPass>(mode);
}

static PassRegistration<CombineDelayChainsPass> pass;

} // namespace pyc
