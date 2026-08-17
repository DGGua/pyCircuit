#pragma once

#include "llvm/ADT/StringRef.h"

namespace pyc {

inline constexpr llvm::StringLiteral kCombPartitionParentIdAttr =
    "pyc.partition.parent_id";
inline constexpr llvm::StringLiteral kCombPartitionPartIdAttr =
    "pyc.partition.part_id";
inline constexpr llvm::StringLiteral kCombPartitionPartCountAttr =
    "pyc.partition.part_count";
inline constexpr llvm::StringLiteral kCombPartitionPlanVersionAttr =
    "pyc.partition.plan_version";
inline constexpr llvm::StringLiteral kCombPartitionWorkAttr =
    "pyc.partition.work";
inline constexpr llvm::StringLiteral kCombPartitionMaxNodesAttr =
    "pyc.partition.max_nodes";
inline constexpr llvm::StringLiteral kCombPartitionPlanVersion =
    "gsim-unified-v2";

inline constexpr llvm::StringLiteral kCombPartitionFunctionPlanAttr =
    "pyc.partition.function_plan";
inline constexpr llvm::StringLiteral kCombPartitionFunctionPartsAttr =
    "pyc.partition.function_parts";
inline constexpr llvm::StringLiteral kCombPartitionFunctionWorkAttr =
    "pyc.partition.function_work";
/// Per-result promoted `pyc.name` values. Empty strings denote ordinary
/// non-observable results; the array length must equal CombOp result arity.
inline constexpr llvm::StringLiteral kCombResultNamesAttr =
    "pyc.comb.result_names";

} // namespace pyc
