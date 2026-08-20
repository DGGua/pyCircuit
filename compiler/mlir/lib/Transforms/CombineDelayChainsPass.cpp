// Form compact delay-line state from proven serial register chains. The
// generated mode preserves the original cycle-balance-only policy; structural
// mode additionally merges equivalent state and admits untagged chains after
// the same sequential proof. Explicit state identity is an opt-in boundary.

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
  int64_t delayTapsCreated = 0;
  int64_t delayTapUsesRewritten = 0;
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

static bool isStatefulTapConsumer(Operation *op) {
  return isa<pyc::RegOp, pyc::DelayLineOp, pyc::FifoOp,
             pyc::ByteMemOp, pyc::SyncMemOp, pyc::SyncMemDPOp,
             pyc::AsyncFifoOp, pyc::CdcSyncOp, pyc::InstanceOp>(op);
}

static Value stripChainAliases(Value value, DelayChainMode mode,
                               bool preserveObservability) {
  while (auto alias = value.getDefiningOp<pyc::AliasOp>()) {
    if (!isTransparentChainAlias(alias, mode, preserveObservability))
      break;
    value = alias.getIn();
  }
  return value;
}

static bool isDelayCandidate(
    pyc::DelayLineOp delay, DelayChainMode mode,
    const StateObservabilityAnalysis &observability,
    bool preserveObservability) {
  if (!delay ||
      (preserveObservability &&
       (shouldKeepStateOptimization(delay) || hasStableStateName(delay) ||
        observability.isPinned(delay.getOperation()))))
    return false;
  if (mode == DelayChainMode::Generated)
    return isCycleBalanceGenerated(delay);
  return true;
}

static void mergeEquivalentStates(
    func::FuncOp function, CombineStats &stats,
    const StateObservabilityAnalysis &observability,
    bool preserveObservability) {
  llvm::SmallVector<pyc::RegOp> regs;
  function.walk([&](pyc::RegOp reg) { regs.push_back(reg); });

  llvm::DenseMap<std::size_t, llvm::SmallVector<pyc::RegOp>> buckets;
  for (pyc::RegOp reg : regs) {
    if (!isStateOptimizationCandidate(reg, DelayChainMode::Structural,
                                      observability,
                                      preserveObservability))
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
    const StateObservabilityAnalysis &observability,
    bool preserveObservability) {
  llvm::SmallVector<pyc::RegOp> regs;
  function.walk([&](pyc::RegOp reg) {
    if (isStateOptimizationCandidate(reg, mode, observability,
                                     preserveObservability))
      regs.push_back(reg);
  });

  llvm::DenseSet<Operation *> erased;
  const bool allowReadOnlyFanout =
      mode == DelayChainMode::Structural && !preserveObservability;
  for (pyc::RegOp tail : llvm::reverse(regs)) {
    if (erased.contains(tail.getOperation()))
      continue;

    llvm::SmallVector<pyc::RegOp> chainFromTail{tail};
    llvm::SmallVector<StateChainLink> linksFromTail;
    pyc::RegOp cursor = tail;
    while (auto link = matchStateChainPredecessor(
               cursor, tail, mode, observability, preserveObservability,
               allowReadOnlyFanout)) {
      if (erased.contains(link->predecessor.getOperation()))
        break;
      linksFromTail.push_back(*link);
      cursor = link->predecessor;
      chainFromTail.push_back(cursor);
    }
    if (chainFromTail.size() < 2)
      continue;

    // Preflight all intermediate fanout before creating the replacement. A
    // second stateful consumer cannot be represented by a read-only tap.
    bool hasIllegalFanout = false;
    for (unsigned i = 1; i < chainFromTail.size() && !hasIllegalFanout; ++i) {
      pyc::RegOp reg = chainFromTail[i];
      const StateChainLink &link = linksFromTail[i - 1];
      Operation *requiredUser = nullptr;
      if (link.aliasesFromConsumerToProducer.empty()) {
        requiredUser = chainFromTail[i - 1].getOperation();
      } else {
        pyc::AliasOp requiredAlias =
            link.aliasesFromConsumerToProducer.back();
        requiredUser = requiredAlias.getOperation();
      }
      for (Operation *user : reg.getQ().getUsers()) {
        if (user != requiredUser && isStatefulTapConsumer(user)) {
          hasIllegalFanout = true;
          break;
        }
      }
      for (auto aliasIt = link.aliasesFromConsumerToProducer.rbegin();
           aliasIt != link.aliasesFromConsumerToProducer.rend() &&
           !hasIllegalFanout;
           ++aliasIt) {
        pyc::AliasOp alias = *aliasIt;
        auto nextAlias = std::next(aliasIt);
        if (nextAlias == link.aliasesFromConsumerToProducer.rend()) {
          requiredUser = chainFromTail[i - 1].getOperation();
        } else {
          pyc::AliasOp nextAliasOp = *nextAlias;
          requiredUser = nextAliasOp.getOperation();
        }
        for (Operation *user : alias.getResult().getUsers()) {
          if (user != requiredUser && isStatefulTapConsumer(user)) {
            hasIllegalFanout = true;
            break;
          }
        }
      }
    }
    if (hasIllegalFanout)
      continue;

    const int64_t depth = static_cast<int64_t>(chainFromTail.size());
    pyc::RegOp head = chainFromTail.back();
    const bool allGenerated = llvm::all_of(
        chainFromTail,
        [](pyc::RegOp reg) { return isCycleBalanceGenerated(reg); });

    // Insert at the original head so the delay and its taps dominate every use
    // that was previously dominated by any state in the chain.
    OpBuilder builder(head);
    auto delay = builder.create<pyc::DelayLineOp>(
        tail.getLoc(), tail.getQ().getType(), head.getClk(), head.getRst(),
        head.getEn(),
        stripChainAliases(head.getNext(), mode, preserveObservability),
        head.getInit());
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

    // A non-tail state may have read-only fanout. Replace each such direct
    // read with a fixed-depth view of the new history, while preserving the
    // unique state-to-state edge that defines the chain. Alias bridges are
    // only accepted when they remain one-use, so they can be erased safely.
    for (unsigned i = 1; i < chainFromTail.size(); ++i) {
      pyc::RegOp reg = chainFromTail[i];
      llvm::SmallVector<OpOperand *, 8> sideUses;
      const StateChainLink &link = linksFromTail[i - 1];
      pyc::AliasOp requiredAlias;
      Operation *requiredUser = nullptr;
      if (link.aliasesFromConsumerToProducer.empty()) {
        requiredUser = chainFromTail[i - 1].getOperation();
      } else {
        requiredAlias = link.aliasesFromConsumerToProducer.back();
        requiredUser = requiredAlias.getOperation();
      }
      for (OpOperand &use : reg.getQ().getUses())
        if (use.getOwner() != requiredUser)
          sideUses.push_back(&use);
      for (auto aliasIt = link.aliasesFromConsumerToProducer.rbegin();
           aliasIt != link.aliasesFromConsumerToProducer.rend(); ++aliasIt) {
        pyc::AliasOp alias = *aliasIt;
        auto nextAlias = std::next(aliasIt);
        if (nextAlias == link.aliasesFromConsumerToProducer.rend()) {
          requiredUser = chainFromTail[i - 1].getOperation();
        } else {
          pyc::AliasOp nextAliasOp = *nextAlias;
          requiredUser = nextAliasOp.getOperation();
        }
        for (OpOperand &use : alias.getResult().getUses())
          if (use.getOwner() != requiredUser)
            sideUses.push_back(&use);
      }
      if (sideUses.empty())
        continue;
      const int64_t tapDepth = static_cast<int64_t>(chainFromTail.size() - i);
      auto tap = builder.create<pyc::DelayTapOp>(
          tail.getLoc(), delay.getQ().getType(), delay.getQ());
      tap->setAttr("depth", builder.getI64IntegerAttr(tapDepth));
      tap->setAttr("pyc.optimized_by",
                   builder.getStringAttr("combine_delay_chain_tap"));
      ++stats.delayTapsCreated;
      for (OpOperand *use : sideUses) {
        use->set(tap.getTap());
        ++stats.delayTapUsesRewritten;
      }
    }

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
    const StateObservabilityAnalysis &observability,
    bool preserveObservability) {
  llvm::SmallVector<pyc::DelayLineOp> delays;
  function.walk([&](pyc::DelayLineOp delay) {
    if (isDelayCandidate(delay, mode, observability, preserveObservability))
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

static void writeCombineStats(func::FuncOp function, const CombineStats &stats,
                              bool accumulate, bool cascadeRound,
                              DelayChainMode mode, bool performedMergeRound) {
  auto write = [&](llvm::StringRef name, int64_t value) {
    if (accumulate)
      value += getI64Attr(function, name, 0);
    setI64Attr(function, name, value);
  };
  const int64_t stateOpsAfter =
      stats.delayLinesCreated > stats.delayLinesMerged
          ? stats.delayLinesCreated - stats.delayLinesMerged
          : 0;
  write("pyc.stats.delay_chains_combined", stats.chainsCombined);
  write("pyc.stats.delay_chain_regs_combined", stats.regsCombined);
  write("pyc.stats.delay_chain_aliases_removed", stats.aliasesRemoved);
  write("pyc.stats.delay_chain_delay_lines_created", stats.delayLinesCreated);
  write("pyc.stats.delay_chain_delay_lines_merged", stats.delayLinesMerged);
  write("pyc.stats.delay_chain_state_reads_before", stats.regsCombined);
  write("pyc.stats.delay_chain_state_reads_after", stateOpsAfter);
  write("pyc.stats.delay_chain_state_writes_before", stats.regsCombined);
  write("pyc.stats.delay_chain_state_writes_after", stateOpsAfter);
  write("pyc.stats.state_opt_regs_merged", stats.stateRegsMerged);
  write("pyc.stats.state_opt_reg_bits_removed", stats.stateRegBitsRemoved);
  write("pyc.stats.state_opt_structural_chains_combined",
        stats.structuralChainsCombined);
  write("pyc.stats.state_opt_structural_chain_regs_combined",
        stats.structuralRegsCombined);
  write("pyc.stats.state_opt_cascade_regs_merged",
        cascadeRound && performedMergeRound ? stats.stateRegsMerged : 0);
  write("pyc.stats.state_opt_merge_rounds",
        mode == DelayChainMode::Structural && performedMergeRound ? 1 : 0);
  write("pyc.stats.delay_chain_taps_created", stats.delayTapsCreated);
  write("pyc.stats.delay_chain_tap_uses_rewritten",
        stats.delayTapUsesRewritten);
}

struct CombineDelayChainsPass
    : public PassWrapper<CombineDelayChainsPass,
                         OperationPass<func::FuncOp>> {
  MLIR_DEFINE_EXPLICIT_INTERNAL_INLINE_TYPE_ID(CombineDelayChainsPass)

  CombineDelayChainsPass() = default;
  CombineDelayChainsPass(const CombineDelayChainsPass &other)
      : PassWrapper(other) {}
  CombineDelayChainsPass(DelayChainMode mode, bool accumulateStats,
                         bool cascadeRound, bool preserveObservability,
                         bool mergeOnly, bool skipMerge) {
    modeOption = stringifyDelayChainMode(mode).str();
    accumulateStatsOption = accumulateStats;
    cascadeRoundOption = cascadeRound;
    preserveObservabilityOption = preserveObservability;
    mergeOnlyOption = mergeOnly;
    skipMergeOption = skipMerge;
  }

  StringRef getArgument() const override { return "pyc-combine-delay-chains"; }
  StringRef getDescription() const override {
    return "Merge equivalent state and form proven fixed-depth delay lines";
  }

  Option<std::string> modeOption{
      *this, "mode",
      llvm::cl::desc("Candidate policy: generated|structural"),
      llvm::cl::init("generated")};
  Option<bool> accumulateStatsOption{
      *this, "accumulate-stats",
      llvm::cl::desc("Add rewrite statistics to an earlier optimization round"),
      llvm::cl::init(false)};
  Option<bool> cascadeRoundOption{
      *this, "cascade-round",
      llvm::cl::desc("Attribute equivalent-state merges to cascade refinement"),
      llvm::cl::init(false)};
  Option<bool> preserveObservabilityOption{
      *this, "preserve-observability",
      llvm::cl::desc("Keep named/debug/probe/trace state identities"),
      llvm::cl::init(false)};
  Option<bool> mergeOnlyOption{
      *this, "merge-only",
      llvm::cl::desc("Only merge equivalent registers; do not form histories"),
      llvm::cl::init(false)};
  Option<bool> skipMergeOption{
      *this, "skip-merge",
      llvm::cl::desc("Form/share histories without equivalent-register merge"),
      llvm::cl::init(false)};

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
    // Generated mode is the compatibility policy and always retains its
    // historical observability boundaries. Structural mode is performance-
    // first unless preservation is explicitly requested.
    const bool preserveObservability =
        *mode == DelayChainMode::Generated || preserveObservabilityOption;
    StateObservabilityAnalysis observability(function,
                                              preserveObservability);
    CombineStats stats;
    if (mergeOnlyOption && skipMergeOption) {
      function.emitError()
          << "merge-only and skip-merge cannot both be enabled";
      signalPassFailure();
      return;
    }
    if (*mode == DelayChainMode::Structural && !skipMergeOption)
      mergeEquivalentStates(function, stats, observability,
                            preserveObservability);
    if (!mergeOnlyOption) {
      combineStateChains(function, stats, *mode, observability,
                         preserveObservability);
      shareEquivalentDelayLines(function, stats, *mode, observability,
                                preserveObservability);
    }
    writeCombineStats(function, stats, accumulateStatsOption,
                      cascadeRoundOption, *mode,
                      *mode == DelayChainMode::Structural &&
                          !skipMergeOption);
  }
};

} // namespace

std::unique_ptr<::mlir::Pass>
createCombineDelayChainsPass(DelayChainMode mode, bool accumulateStats,
                             bool cascadeRound,
                             bool preserveObservability, bool mergeOnly,
                             bool skipMerge) {
  return std::make_unique<CombineDelayChainsPass>(mode, accumulateStats,
                                                   cascadeRound,
                                                   preserveObservability,
                                                   mergeOnly, skipMerge);
}

static PassRegistration<CombineDelayChainsPass> pass;

} // namespace pyc
