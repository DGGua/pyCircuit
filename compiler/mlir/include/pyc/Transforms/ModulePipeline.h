#pragma once

#include <cstdint>
#include <optional>

#include "mlir/Dialect/Func/IR/FuncOps.h"
#include "mlir/IR/BuiltinOps.h"
#include "llvm/ADT/StringRef.h"

namespace pyc {

inline constexpr llvm::StringLiteral kModulePipelineSchema =
    "pyc.module_pipeline.v1";
inline constexpr llvm::StringLiteral kModulePipelineSummaryAttr =
    "pyc.module_pipeline.summary";
inline constexpr llvm::StringLiteral kPipelineValidatedAttr =
    "pyc.pipeline.validated";
inline constexpr llvm::StringLiteral kPipelineStageDagAttr =
    "pyc.pipeline.stage_dag";

// Stable failure classes for consumers of module-pipeline diagnostics.
enum class ModulePipelineError {
  Cycle,
  MissingCalleeBody,
  MultipleDriver,
  UnsupportedEdge,
  RewriteInvariant,
};

// Aggregate analysis results stored on the builtin module. The first version
// deliberately distinguishes a validated scheduling DAG from a physical
// func.func rewrite.
struct ModulePipelineSummary {
  int64_t functionCount = 0;
  int64_t coarseSccCount = 0;
  int64_t falseSccCount = 0;
  int64_t trueCycleCount = 0;
  int64_t stateCutCount = 0;
  bool rewritten = false;
};

bool isPipelineValidatedFunc(mlir::func::FuncOp func);
std::optional<ModulePipelineSummary>
readModulePipelineSummary(mlir::ModuleOp module);

} // namespace pyc
