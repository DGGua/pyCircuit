#pragma once

#include "pyc/Dialect/PYC/PYCOps.h"
#include "pyc/Transforms/Passes.h"

#include "mlir/Dialect/Func/IR/FuncOps.h"
#include "mlir/IR/Builders.h"
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
/// True when `pyc.observe_lazy` marks an undeclared name as on-read lookup.
bool isObserveLazy(mlir::Operation *op);
/// True for a non-lazy, non-cycle-balance `pyc.name` that must stay an eager probe.
bool hasStableStateName(mlir::Operation *op);

/// True when `op` carries an identity that probe/VCD/`dut.read` may consume.
/// `cycle_balance` auto-names are excluded; other observation attrs still count.
bool hasExternalObservationIdentity(mlir::Operation *op);

/// True when the state op or any alias of `q` has an external observation
/// identity that must remain readable after a rewrite.
bool stateHasExternalObservation(mlir::Operation *state, mlir::Value q);

/// Copy name/debug/probe/trace/observable attrs, skipping cycle-balance names.
void copyExternalObservationAttrs(mlir::Operation *from, mlir::Operation *to);

/// Attach a read-only alias of `source` that carries `attrSource`'s external
/// observation identity. Used when a rewrite deletes or replaces the physical
/// state but must leave a value for external reads.
pyc::AliasOp materializeObservationAlias(mlir::OpBuilder &builder,
                                         mlir::Location loc, mlir::Value source,
                                         mlir::Operation *attrSource);

/// Materialize observation aliases for identities that live on `oldState`
/// itself. Aliases of `oldQ` are left to RAUW or explicit remap of erased
/// aliases.
void remapStateOpIdentity(mlir::OpBuilder &builder, mlir::Operation *oldState,
                          mlir::Value newSource);

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

bool isStateOptimizationCandidate(pyc::RegOp reg, DelayChainMode mode);
bool isTransparentChainAlias(pyc::AliasOp alias, DelayChainMode mode);

struct StateChainLink {
  pyc::RegOp predecessor;
  llvm::SmallVector<pyc::AliasOp> aliasesFromConsumerToProducer;
};

std::optional<StateChainLink>
matchStateChainPredecessor(pyc::RegOp consumer, pyc::RegOp keyReg,
                           DelayChainMode mode,
                           bool allowReadOnlyFanout = false);

bool equivalentRegisterState(pyc::RegOp lhs, pyc::RegOp rhs);
bool equivalentDelayLineState(pyc::DelayLineOp lhs, pyc::DelayLineOp rhs);
std::size_t registerStateHash(pyc::RegOp reg);
std::size_t delayLineStateHash(pyc::DelayLineOp delay);

} // namespace pyc
