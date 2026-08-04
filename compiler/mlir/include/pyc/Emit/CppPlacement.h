#pragma once

#include "pyc/Dialect/PYC/PYCOps.h"

#include "mlir/Dialect/Func/IR/FuncOps.h"
#include "mlir/IR/BuiltinOps.h"
#include "mlir/IR/Value.h"
#include <cstdint>
#include <optional>
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"

namespace pyc {

inline constexpr llvm::StringLiteral kCppStorageAttr = "pyc.cpp.storage";
inline constexpr llvm::StringLiteral kCppOwnerAttr = "pyc.cpp.owner";
inline constexpr llvm::StringLiteral kCppMethodAttr = "pyc.cpp.method";

enum class CppStorageKind { Struct, Local };

struct CppPlacementSummary {
  unsigned structMembers = 0;
  /// Comb wires localized as function-local Wire<> purely by comb boundary:
  /// defined inside a comb, not a comb result, not a block arg, no escaping use.
  unsigned localInMethod = 0;
  /// Comb-region values without a defining op (e.g. block args) kept on the struct.
  unsigned probePinnedStruct = 0;
  /// Comb wires that would be local by boundary, but are used across part methods
  /// and therefore promoted to struct members.
  unsigned crossPartPromoted = 0;
  /// Cross-part localizable wires under the chosen schedule/cut (or fallback).
  unsigned scheduledCrossMethod = 0;
  /// Weighted cut cost for the chosen scheduled partition.
  uint64_t scheduledCutWeight = 0;
};

/// Read storage decision for a value (default struct).
CppStorageKind getValueCppStorage(mlir::Value v);

/// Owner method name for local values (empty if struct or unknown).
llvm::StringRef getValueCppOwner(mlir::Value v);

inline constexpr llvm::StringLiteral kCppPlacementSummaryAttr = "pyc.cpp.placement_summary";
inline constexpr llvm::StringLiteral kCppCombChunkNodesAttr = "pyc.cpp.comb_chunk_nodes";

void setModuleCombChunkNodes(mlir::ModuleOp module, unsigned combChunkNodes);

/// Chunk size chosen by `pyc-cpp-placement` (emit + localization read this).
std::optional<unsigned> getModuleCombChunkNodes(mlir::ModuleOp module);

/// Decide struct vs method-local storage for every value in \p f and annotate the IR.
/// Returns placement statistics consumed by the build profile JSON.
CppPlacementSummary runCppMemberPlacement(mlir::func::FuncOp f, unsigned combChunkNodes);

void setFuncPlacementSummary(mlir::func::FuncOp f, const CppPlacementSummary &summary);

/// Read per-function summary written by `pyc-cpp-placement`.
std::optional<CppPlacementSummary> getFuncPlacementSummary(mlir::func::FuncOp f);

CppPlacementSummary accumulateModulePlacementSummary(mlir::ModuleOp module);

/// Per-emission state for lazy local declarations inside a method body.
struct CppEmitterPlacementState {
  llvm::StringRef currentMethod;
  llvm::DenseSet<mlir::Value> declaredLocals;

  void beginMethod(llvm::StringRef methodName) {
    currentMethod = methodName;
    declaredLocals.clear();
  }

  bool emitLocalDeclIfNeeded(mlir::Value v, mlir::Type ty, llvm::StringRef name,
                           llvm::raw_ostream &os, unsigned indentSpaces = 4);

  /// Emit `name = expr` or `Wire<w> name = expr` for method-local SSA results.
  void emitValueAssign(mlir::Value result, mlir::Type ty, llvm::StringRef name, llvm::StringRef expr,
                       llvm::raw_ostream &os, unsigned indentSpaces = 4);
};

} // namespace pyc
