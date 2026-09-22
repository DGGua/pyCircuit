#pragma once

#include "mlir/Dialect/Func/IR/FuncOps.h"
#include "mlir/IR/BuiltinOps.h"
#include "mlir/Support/LogicalResult.h"
#include "llvm/Support/raw_ostream.h"

#include <string>

namespace pyc {

struct CppEmitterOptions {
  enum class SplitMode {
    None,
    Module,
  };

  /// Runtime update policy for fused `pyc.comb` regions.
  ///
  /// Always is the reference schedule. Guarded snapshots every direct input.
  /// Dirty also propagates semantic output changes through local direct-comb
  /// fanout while conservatively polling non-comb boundaries.
  enum class CombUpdateMode {
    Always,
    Guarded,
    Dirty,
  };

  SplitMode splitMode = SplitMode::None;
  unsigned shardThresholdLines = 120000;
  unsigned shardThresholdBytes = 4 * 1024 * 1024;
  // Chunk full-topology eval bodies to avoid mega-functions that are expensive
  // for downstream C++ compilers.
  unsigned evalTopoChunkNodes = 256;
  // Chunk fused comb helpers to avoid single mega-functions that dominate
  // downstream C++ TU cost even after file sharding.
  unsigned combChunkNodes = 256;
  CombUpdateMode combUpdateMode = CombUpdateMode::Dirty;
  std::string probePlanPath{};
};

::mlir::LogicalResult emitCpp(::mlir::ModuleOp module, ::llvm::raw_ostream &os,
                              const CppEmitterOptions &opts = {});

::mlir::LogicalResult emitCppFunc(::mlir::ModuleOp module, ::mlir::func::FuncOp f, ::llvm::raw_ostream &os,
                                  const CppEmitterOptions &opts = {});

} // namespace pyc
