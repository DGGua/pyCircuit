#pragma once

#include "pyc/Dialect/PYC/PYCOps.h"
#include "pyc/Transforms/Passes.h"

#include "mlir/Dialect/Func/IR/FuncOps.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"

#include <cstddef>
#include <optional>

namespace pyc {

std::optional<DelayChainMode> parseDelayChainMode(llvm::StringRef value);
llvm::StringRef stringifyDelayChainMode(DelayChainMode mode);

bool isCycleBalanceGenerated(mlir::Operation *op);
bool shouldKeepStateOptimization(mlir::Operation *op);
bool hasStableStateName(mlir::Operation *op);

/// Identifies state whose logical identity must survive optimization. Value
/// observability is handled separately by the rewrite's use/fanout proof.
class StateObservabilityAnalysis {
public:
  explicit StateObservabilityAnalysis(mlir::func::FuncOp function,
                                      bool analyze = true);

  bool isPinned(mlir::Operation *op) const { return pinned.contains(op); }

private:
  llvm::DenseSet<mlir::Operation *> pinned;
};

mlir::Value stripStateAliases(mlir::Value value);
bool equivalentStateValue(mlir::Value lhs, mlir::Value rhs);

bool isStateOptimizationCandidate(pyc::RegOp reg, DelayChainMode mode,
                                  const StateObservabilityAnalysis &observability,
                                  bool preserveObservability = true);
bool isTransparentChainAlias(pyc::AliasOp alias, DelayChainMode mode,
                             bool preserveObservability = true);

struct StateChainLink {
  pyc::RegOp predecessor;
  llvm::SmallVector<pyc::AliasOp> aliasesFromConsumerToProducer;
};

std::optional<StateChainLink>
matchStateChainPredecessor(pyc::RegOp consumer, pyc::RegOp keyReg,
                           DelayChainMode mode,
                           const StateObservabilityAnalysis &observability,
                           bool preserveObservability = true);

bool equivalentRegisterState(pyc::RegOp lhs, pyc::RegOp rhs);
bool equivalentDelayLineState(pyc::DelayLineOp lhs, pyc::DelayLineOp rhs);
std::size_t registerStateHash(pyc::RegOp reg);
std::size_t delayLineStateHash(pyc::DelayLineOp delay);

} // namespace pyc
