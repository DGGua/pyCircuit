#include "pyc/Transforms/StateOptimization.h"

#include "mlir/IR/BuiltinAttributes.h"
#include "llvm/ADT/Hashing.h"
#include "llvm/ADT/SmallVector.h"

using namespace mlir;

namespace pyc {

namespace {

static bool hasObservationAttribute(Operation *op) {
  for (NamedAttribute attr : op->getAttrs()) {
    llvm::StringRef name = attr.getName().strref();
    if (name.starts_with("pyc.probe") || name.starts_with("pyc.trace") ||
        name == "pyc.observable")
      return true;
  }
  return false;
}

static std::size_t opaqueHash(const void *ptr) {
  return static_cast<std::size_t>(llvm::hash_value(ptr));
}

static std::size_t semanticValueHash(Value value) {
  value = stripStateAliases(value);
  if (auto constant = value.getDefiningOp<pyc::ConstantOp>()) {
    Attribute literal = constant->getAttr("value");
    return static_cast<std::size_t>(llvm::hash_combine(
        value.getType().getAsOpaquePointer(), literal.getAsOpaquePointer(), 1));
  }
  return static_cast<std::size_t>(llvm::hash_combine(
      value.getType().getAsOpaquePointer(), value.getAsOpaquePointer(), 0));
}

} // namespace

std::optional<DelayChainMode> parseDelayChainMode(llvm::StringRef value) {
  if (value == "generated")
    return DelayChainMode::Generated;
  if (value == "structural")
    return DelayChainMode::Structural;
  return std::nullopt;
}

llvm::StringRef stringifyDelayChainMode(DelayChainMode mode) {
  switch (mode) {
  case DelayChainMode::Generated:
    return "generated";
  case DelayChainMode::Structural:
    return "structural";
  }
  llvm_unreachable("unknown delay-chain mode");
}

bool isCycleBalanceGenerated(Operation *op) {
  if (!op)
    return false;
  auto generated = op->getAttrOfType<StringAttr>("pyc.generated");
  return generated && generated.getValue() == "cycle_balance";
}

bool shouldKeepStateOptimization(Operation *op) {
  if (!op)
    return false;
  if (auto keep = op->getAttrOfType<BoolAttr>("pyc.debug_keep"))
    return keep.getValue();
  return hasObservationAttribute(op);
}

bool hasStableStateName(Operation *op) {
  if (!op || !op->hasAttrOfType<StringAttr>("pyc.name"))
    return false;
  // Frontend cycle-balance names are explicitly excluded from both probe
  // manifest generation and C++ ProbeRegistry registration.
  return !isCycleBalanceGenerated(op);
}

StateObservabilityAnalysis::StateObservabilityAnalysis(func::FuncOp function) {
  auto inspectState = [&](Operation *state, Value q) {
    if (shouldKeepStateOptimization(state) || hasStableStateName(state))
      pinned.insert(state);

    llvm::SmallVector<Value> worklist{q};
    llvm::DenseSet<Value> seen;
    while (!worklist.empty()) {
      Value value = worklist.pop_back_val();
      if (!seen.insert(value).second)
        continue;
      for (Operation *user : value.getUsers()) {
        auto alias = dyn_cast<pyc::AliasOp>(user);
        if (!alias)
          continue;
        if (shouldKeepStateOptimization(alias) || hasStableStateName(alias))
          pinned.insert(state);
        worklist.push_back(alias.getResult());
      }
    }
  };

  function.walk([&](Operation *op) {
    if (auto reg = dyn_cast<pyc::RegOp>(op)) {
      inspectState(op, reg.getQ());
      return;
    }
    if (auto delay = dyn_cast<pyc::DelayLineOp>(op))
      inspectState(op, delay.getQ());
  });
}

Value stripStateAliases(Value value) {
  while (auto alias = value.getDefiningOp<pyc::AliasOp>())
    value = alias.getIn();
  return value;
}

bool equivalentStateValue(Value lhs, Value rhs) {
  lhs = stripStateAliases(lhs);
  rhs = stripStateAliases(rhs);
  if (lhs == rhs)
    return true;
  if (lhs.getType() != rhs.getType())
    return false;
  auto lhsConstant = lhs.getDefiningOp<pyc::ConstantOp>();
  auto rhsConstant = rhs.getDefiningOp<pyc::ConstantOp>();
  if (!lhsConstant || !rhsConstant)
    return false;
  return lhsConstant->getAttr("value") == rhsConstant->getAttr("value");
}

bool isStateOptimizationCandidate(
    pyc::RegOp reg, DelayChainMode mode,
    const StateObservabilityAnalysis &observability) {
  if (!reg || observability.isPinned(reg.getOperation()))
    return false;
  if (mode == DelayChainMode::Generated)
    return isCycleBalanceGenerated(reg);
  return true;
}

bool isTransparentChainAlias(pyc::AliasOp alias, DelayChainMode mode) {
  if (!alias || shouldKeepStateOptimization(alias) || hasStableStateName(alias))
    return false;
  if (mode == DelayChainMode::Generated)
    return isCycleBalanceGenerated(alias);
  return true;
}

std::optional<StateChainLink>
matchStateChainPredecessor(pyc::RegOp consumer, pyc::RegOp keyReg,
                           DelayChainMode mode,
                           const StateObservabilityAnalysis &observability) {
  Value value = consumer.getNext();
  StateChainLink link;
  while (auto alias = value.getDefiningOp<pyc::AliasOp>()) {
    if (!isTransparentChainAlias(alias, mode))
      return std::nullopt;
    link.aliasesFromConsumerToProducer.push_back(alias);
    value = alias.getIn();
  }

  auto predecessor = value.getDefiningOp<pyc::RegOp>();
  if (!predecessor ||
      !isStateOptimizationCandidate(predecessor, mode, observability))
    return std::nullopt;
  if (predecessor.getQ().getType() != keyReg.getQ().getType() ||
      predecessor.getClk() != keyReg.getClk() ||
      predecessor.getRst() != keyReg.getRst() ||
      !equivalentStateValue(predecessor.getEn(), keyReg.getEn()) ||
      !equivalentStateValue(predecessor.getInit(), keyReg.getInit()))
    return std::nullopt;

  Value expectedProducer = predecessor.getQ();
  for (pyc::AliasOp alias :
       llvm::reverse(link.aliasesFromConsumerToProducer)) {
    if (!expectedProducer.hasOneUse() ||
        *expectedProducer.user_begin() != alias.getOperation())
      return std::nullopt;
    expectedProducer = alias.getResult();
  }
  if (!expectedProducer.hasOneUse() ||
      *expectedProducer.user_begin() != consumer.getOperation())
    return std::nullopt;

  link.predecessor = predecessor;
  return link;
}

bool equivalentRegisterState(pyc::RegOp lhs, pyc::RegOp rhs) {
  return lhs.getQ().getType() == rhs.getQ().getType() &&
         lhs.getClk() == rhs.getClk() && lhs.getRst() == rhs.getRst() &&
         equivalentStateValue(lhs.getEn(), rhs.getEn()) &&
         equivalentStateValue(lhs.getNext(), rhs.getNext()) &&
         equivalentStateValue(lhs.getInit(), rhs.getInit());
}

bool equivalentDelayLineState(pyc::DelayLineOp lhs, pyc::DelayLineOp rhs) {
  auto lhsDepth = lhs->getAttrOfType<IntegerAttr>("depth");
  auto rhsDepth = rhs->getAttrOfType<IntegerAttr>("depth");
  return lhsDepth && rhsDepth && lhsDepth == rhsDepth &&
         lhs.getQ().getType() == rhs.getQ().getType() &&
         lhs.getClk() == rhs.getClk() && lhs.getRst() == rhs.getRst() &&
         equivalentStateValue(lhs.getEn(), rhs.getEn()) &&
         equivalentStateValue(lhs.getNext(), rhs.getNext()) &&
         equivalentStateValue(lhs.getInit(), rhs.getInit());
}

std::size_t registerStateHash(pyc::RegOp reg) {
  return static_cast<std::size_t>(llvm::hash_combine(
      opaqueHash(reg.getQ().getType().getAsOpaquePointer()),
      opaqueHash(reg.getClk().getAsOpaquePointer()),
      opaqueHash(reg.getRst().getAsOpaquePointer()),
      semanticValueHash(reg.getEn()), semanticValueHash(reg.getNext()),
      semanticValueHash(reg.getInit())));
}

std::size_t delayLineStateHash(pyc::DelayLineOp delay) {
  auto depth = delay->getAttrOfType<IntegerAttr>("depth");
  return static_cast<std::size_t>(llvm::hash_combine(
      opaqueHash(delay.getQ().getType().getAsOpaquePointer()),
      depth ? depth.getInt() : 0,
      opaqueHash(delay.getClk().getAsOpaquePointer()),
      opaqueHash(delay.getRst().getAsOpaquePointer()),
      semanticValueHash(delay.getEn()), semanticValueHash(delay.getNext()),
      semanticValueHash(delay.getInit())));
}

} // namespace pyc
