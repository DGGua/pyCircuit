#pragma once

#include "pyc/Dialect/PYC/PYCOps.h"

#include "mlir/Dialect/Func/IR/FuncOps.h"
#include "mlir/IR/BuiltinOps.h"
#include "mlir/IR/Value.h"
#include <optional>
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"

namespace pyc {

inline constexpr llvm::StringLiteral kCppStorageAttr = "pyc.cpp.storage";
inline constexpr llvm::StringLiteral kCppOwnerAttr = "pyc.cpp.owner";

enum class CppStorageKind { Struct, Local };

struct CppPlacementSummary {
  unsigned structMembers = 0;
  /// Comb wires localized as function-local Wire<> (all uses in one eval_comb_*).
  unsigned localInMethod = 0;
  unsigned promotedCrossMethod = 0;
  /// Comb-region values without a defining op (e.g. block args) kept on the struct.
  unsigned probePinnedStruct = 0;
};

/// Read storage decision for a value (default struct).
CppStorageKind getValueCppStorage(mlir::Value v);

/// Owner method name for local values (empty if struct or unknown).
llvm::StringRef getValueCppOwner(mlir::Value v);

/// Stable sort key for top-level `pyc.comb` ops (placement + emit must agree).
std::string combSortKey(pyc::CombOp comb);

/// Sort combs by \p combSortKey (in-place).
void sortCombsByStableKey(llvm::SmallVectorImpl<pyc::CombOp> &combs);

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
