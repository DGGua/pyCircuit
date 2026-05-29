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
inline constexpr llvm::StringLiteral kCppShardAttr = "pyc.cpp.shard";

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

void setValueCppPlacement(mlir::Value v, CppStorageKind kind, llvm::StringRef owner,
                           llvm::StringRef shard = {});

/// Stable sort key for top-level `pyc.comb` ops (placement + emit must agree).
std::string combSortKey(pyc::CombOp comb);

/// Sort combs by \p combSortKey (in-place).
void sortCombsByStableKey(llvm::SmallVectorImpl<pyc::CombOp> &combs);

inline constexpr llvm::StringLiteral kCppPlacementSummaryAttr = "pyc.cpp.placement_summary";
inline constexpr llvm::StringLiteral kCppCombChunkNodesAttr = "pyc.cpp.comb_chunk_nodes";

void setModuleCombChunkNodes(mlir::ModuleOp module, unsigned combChunkNodes);

/// Chunk size chosen by `pyc-cpp-placement` (emit + localization read this).
std::optional<unsigned> getModuleCombChunkNodes(mlir::ModuleOp module);

/// Internal phase 1: assign `pyc.cpp.owner` (matches CppEmitter eval_comb_* chunking).
void scheduleCppCombMethods(mlir::func::FuncOp f, unsigned combChunkNodes);

/// Internal phase 2: struct vs method-local using owners from scheduleCppCombMethods.
CppPlacementSummary localizeCppCombMembers(mlir::func::FuncOp f, unsigned combChunkNodes);

/// Used by `pyc-cpp-placement` when member localization is enabled.
CppPlacementSummary runCppMemberPlacement(mlir::func::FuncOp f, unsigned combChunkNodes);

void setFuncPlacementSummary(mlir::func::FuncOp f, const CppPlacementSummary &summary);

/// Read per-function summary written by `pyc-cpp-placement` (localize on).
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

  bool shouldEmitStructMember(mlir::Value v) const {
    return getValueCppStorage(v) == CppStorageKind::Struct;
  }

  bool emitLocalDeclIfNeeded(mlir::Value v, mlir::Type ty, llvm::StringRef name,
                           llvm::raw_ostream &os, unsigned indentSpaces = 4);

  /// Emit `name = expr` or `Wire<w> name = expr` for method-local SSA results.
  void emitValueAssign(mlir::Value result, mlir::Type ty, llvm::StringRef name, llvm::StringRef expr,
                       llvm::raw_ostream &os, unsigned indentSpaces = 4);
};

} // namespace pyc
