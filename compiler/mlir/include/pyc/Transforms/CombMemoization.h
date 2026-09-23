#pragma once

#include "mlir/IR/Types.h"

namespace mlir {
class Operation;
}

namespace pyc {

/// The explicit deterministic operation contract shared by pyc.comb fusion and
/// the memoization legality gate. MemoryEffectFree alone is insufficient:
/// newly introduced pure operations must opt in here deliberately.
bool isMemoizableCombOperation(::mlir::Operation *op);

/// True when generated runtime equality covers the complete value.
bool isMemoizableCombType(::mlir::Type type);

} // namespace pyc
