// Recognize frontend-owned cycle-balance register chains and replace each
// unobservable chain with a first-class pyc.delay_line. Identical generated
// histories are shared only after their complete sequential semantics match.

#include "pyc/Transforms/Passes.h"

#include "pyc/Dialect/PYC/PYCOps.h"

#include "mlir/Dialect/Func/IR/FuncOps.h"
#include "mlir/IR/Builders.h"
#include "mlir/Pass/Pass.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallVector.h"

#include <optional>

using namespace mlir;

namespace pyc {
namespace {

constexpr llvm::StringLiteral kGeneratedAttr = "pyc.generated";
constexpr llvm::StringLiteral kCycleBalance = "cycle_balance";
constexpr llvm::StringLiteral kOptimizedByAttr = "pyc.optimized_by";
constexpr llvm::StringLiteral kCombineDelayChains = "combine_delay_chains";
constexpr llvm::StringLiteral kSourceRegCountAttr = "pyc.source_reg_count";
constexpr llvm::StringLiteral kSharedChainCountAttr = "pyc.shared_chain_count";

struct CombineStats {
  int64_t chainsCombined = 0;
  int64_t regsCombined = 0;
  int64_t aliasesRemoved = 0;
  int64_t delayLinesCreated = 0;
  int64_t delayLinesMerged = 0;
};

static int64_t getI64Attr(Operation *op, llvm::StringRef name, int64_t fallback) {
  if (auto value = op->getAttrOfType<IntegerAttr>(name))
    return value.getInt();
  return fallback;
}

static void setI64Attr(Operation *op, llvm::StringRef name, int64_t value) {
  OpBuilder builder(op->getContext());
  op->setAttr(name, builder.getI64IntegerAttr(value));
}

static bool isCycleBalanceGenerated(Operation *op) {
  if (!op)
    return false;
  auto generated = op->getAttrOfType<StringAttr>(kGeneratedAttr);
  return generated && generated.getValue() == kCycleBalance;
}

static bool shouldKeep(Operation *op) {
  if (!op)
    return false;
  if (auto keep = op->getAttrOfType<BoolAttr>("pyc.debug_keep"))
    return keep.getValue();
  return false;
}

static Value stripGeneratedAliases(Value value) {
  while (auto alias = value.getDefiningOp<pyc::AliasOp>()) {
    if (!isCycleBalanceGenerated(alias) || shouldKeep(alias))
      break;
    value = alias.getIn();
  }
  return value;
}

static bool equivalentValue(Value lhs, Value rhs) {
  lhs = stripGeneratedAliases(lhs);
  rhs = stripGeneratedAliases(rhs);
  if (lhs == rhs)
    return true;
  if (lhs.getType() != rhs.getType())
    return false;
  auto lhsConst = lhs.getDefiningOp<pyc::ConstantOp>();
  auto rhsConst = rhs.getDefiningOp<pyc::ConstantOp>();
  if (!lhsConst || !rhsConst)
    return false;
  return lhsConst->getAttr("value") == rhsConst->getAttr("value");
}

struct ChainLink {
  pyc::RegOp predecessor;
  llvm::SmallVector<pyc::AliasOp> aliasesFromConsumerToProducer;
};

static std::optional<ChainLink> matchPredecessor(pyc::RegOp consumer, pyc::RegOp keyReg) {
  Value value = consumer.getNext();
  ChainLink link;
  while (auto alias = value.getDefiningOp<pyc::AliasOp>()) {
    if (!isCycleBalanceGenerated(alias) || shouldKeep(alias))
      return std::nullopt;
    link.aliasesFromConsumerToProducer.push_back(alias);
    value = alias.getIn();
  }

  auto predecessor = value.getDefiningOp<pyc::RegOp>();
  if (!predecessor || !isCycleBalanceGenerated(predecessor) || shouldKeep(predecessor))
    return std::nullopt;
  if (predecessor.getQ().getType() != keyReg.getQ().getType() ||
      predecessor.getClk() != keyReg.getClk() || predecessor.getRst() != keyReg.getRst() ||
      !equivalentValue(predecessor.getEn(), keyReg.getEn()) ||
      !equivalentValue(predecessor.getInit(), keyReg.getInit()))
    return std::nullopt;

  // Prove that the predecessor and every transparent alias are used only by
  // this one link. This is the observation/fanout gate for final-output-only
  // delay lines.
  Value expectedProducer = predecessor.getQ();
  for (pyc::AliasOp alias : llvm::reverse(link.aliasesFromConsumerToProducer)) {
    if (!expectedProducer.hasOneUse() || *expectedProducer.user_begin() != alias.getOperation())
      return std::nullopt;
    expectedProducer = alias.getResult();
  }
  if (!expectedProducer.hasOneUse() || *expectedProducer.user_begin() != consumer.getOperation())
    return std::nullopt;

  link.predecessor = predecessor;
  return link;
}

static bool sameDelayKey(pyc::DelayLineOp lhs, pyc::DelayLineOp rhs) {
  auto lhsDepth = lhs->getAttrOfType<IntegerAttr>("depth");
  auto rhsDepth = rhs->getAttrOfType<IntegerAttr>("depth");
  return lhsDepth && rhsDepth && lhsDepth == rhsDepth && lhs.getQ().getType() == rhs.getQ().getType() &&
         lhs.getClk() == rhs.getClk() && lhs.getRst() == rhs.getRst() &&
         equivalentValue(lhs.getEn(), rhs.getEn()) && equivalentValue(lhs.getNext(), rhs.getNext()) &&
         equivalentValue(lhs.getInit(), rhs.getInit());
}

static void combineGeneratedChains(func::FuncOp function, CombineStats &stats) {
  llvm::SmallVector<pyc::RegOp> regs;
  function.walk([&](pyc::RegOp reg) {
    if (isCycleBalanceGenerated(reg) && !shouldKeep(reg))
      regs.push_back(reg);
  });

  llvm::DenseSet<Operation *> erased;
  for (pyc::RegOp tail : llvm::reverse(regs)) {
    if (erased.contains(tail.getOperation()))
      continue;

    llvm::SmallVector<pyc::RegOp> chainFromTail;
    llvm::SmallVector<ChainLink> linksFromTail;
    chainFromTail.push_back(tail);
    pyc::RegOp cursor = tail;
    while (auto link = matchPredecessor(cursor, tail)) {
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
    OpBuilder builder(tail);
    auto delay = builder.create<pyc::DelayLineOp>(
        tail.getLoc(), tail.getQ().getType(), head.getClk(), head.getRst(), head.getEn(),
        stripGeneratedAliases(head.getNext()), head.getInit());
    delay->setAttr("depth", builder.getI64IntegerAttr(depth));
    delay->setAttr(kGeneratedAttr, builder.getStringAttr(kCycleBalance));
    delay->setAttr(kOptimizedByAttr, builder.getStringAttr(kCombineDelayChains));
    delay->setAttr(kSourceRegCountAttr, builder.getI64IntegerAttr(depth));
    delay->setAttr(kSharedChainCountAttr, builder.getI64IntegerAttr(1));

    ++stats.chainsCombined;
    stats.regsCombined += depth;
    ++stats.delayLinesCreated;
    for (const ChainLink &link : linksFromTail)
      stats.aliasesRemoved += static_cast<int64_t>(link.aliasesFromConsumerToProducer.size());

    tail.getQ().replaceAllUsesWith(delay.getQ());

    // Erase from the consumer end. Erasing the consumer releases the closest
    // alias; erasing that alias chain releases the predecessor register.
    for (unsigned i = 0; i < linksFromTail.size(); ++i) {
      pyc::RegOp reg = chainFromTail[i];
      erased.insert(reg.getOperation());
      reg.erase();
      for (pyc::AliasOp alias : linksFromTail[i].aliasesFromConsumerToProducer)
        alias.erase();
    }
    erased.insert(head.getOperation());
    head.erase();
  }
}

static void shareEquivalentDelayLines(func::FuncOp function, CombineStats &stats) {
  // Stateful operations are intentionally not ordinary CSE candidates. Share
  // generated delay lines explicitly when all semantic operands match.
  llvm::SmallVector<pyc::DelayLineOp> delays;
  function.walk([&](pyc::DelayLineOp delay) {
    if (isCycleBalanceGenerated(delay) && !shouldKeep(delay))
      delays.push_back(delay);
  });
  for (unsigned i = 0; i < delays.size(); ++i) {
    if (!delays[i])
      continue;
    for (unsigned j = i + 1; j < delays.size(); ++j) {
      if (!delays[j] || !sameDelayKey(delays[i], delays[j]))
        continue;

      const int64_t lhsRegs = getI64Attr(delays[i], kSourceRegCountAttr, 0);
      const int64_t rhsRegs = getI64Attr(delays[j], kSourceRegCountAttr, 0);
      const int64_t lhsChains = getI64Attr(delays[i], kSharedChainCountAttr, 0);
      const int64_t rhsChains = getI64Attr(delays[j], kSharedChainCountAttr, 0);
      if (lhsRegs || rhsRegs)
        setI64Attr(delays[i], kSourceRegCountAttr, lhsRegs + rhsRegs);
      if (lhsChains || rhsChains)
        setI64Attr(delays[i], kSharedChainCountAttr, lhsChains + rhsChains);

      delays[j].getQ().replaceAllUsesWith(delays[i].getQ());
      delays[j].erase();
      delays[j] = nullptr;
      ++stats.delayLinesMerged;
    }
  }
}

static void writeCombineStats(func::FuncOp function, const CombineStats &stats) {
  // A re-optimized intermediate file may merge delay lines created by an
  // earlier pipeline run. Those merges are observable, but cannot make this
  // run's state-primitive count negative.
  const int64_t stateOpsAfter = stats.delayLinesCreated > stats.delayLinesMerged
                                    ? stats.delayLinesCreated - stats.delayLinesMerged
                                    : 0;
  setI64Attr(function, "pyc.stats.delay_chains_combined", stats.chainsCombined);
  setI64Attr(function, "pyc.stats.delay_chain_regs_combined", stats.regsCombined);
  setI64Attr(function, "pyc.stats.delay_chain_aliases_removed", stats.aliasesRemoved);
  setI64Attr(function, "pyc.stats.delay_chain_delay_lines_created", stats.delayLinesCreated);
  setI64Attr(function, "pyc.stats.delay_chain_delay_lines_merged", stats.delayLinesMerged);
  setI64Attr(function, "pyc.stats.delay_chain_state_reads_before", stats.regsCombined);
  setI64Attr(function, "pyc.stats.delay_chain_state_reads_after", stateOpsAfter);
  setI64Attr(function, "pyc.stats.delay_chain_state_writes_before", stats.regsCombined);
  setI64Attr(function, "pyc.stats.delay_chain_state_writes_after", stateOpsAfter);
}

struct CombineDelayChainsPass
    : public PassWrapper<CombineDelayChainsPass, OperationPass<func::FuncOp>> {
  MLIR_DEFINE_EXPLICIT_INTERNAL_INLINE_TYPE_ID(CombineDelayChainsPass)

  StringRef getArgument() const override { return "pyc-combine-delay-chains"; }
  StringRef getDescription() const override {
    return "Combine generated serial cycle-balance registers into fixed-depth delay lines";
  }

  void runOnOperation() override {
    func::FuncOp function = getOperation();
    CombineStats stats;
    combineGeneratedChains(function, stats);
    shareEquivalentDelayLines(function, stats);
    writeCombineStats(function, stats);
  }
};

} // namespace

std::unique_ptr<::mlir::Pass> createCombineDelayChainsPass() {
  return std::make_unique<CombineDelayChainsPass>();
}

static PassRegistration<CombineDelayChainsPass> pass;

} // namespace pyc
