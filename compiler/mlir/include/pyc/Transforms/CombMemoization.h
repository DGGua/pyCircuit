#pragma once

namespace mlir {
class Operation;
}

namespace pyc {

/// Operations FuseCombPass may fold into one pyc.comb region.
/// MemoryEffectFree alone is insufficient: a new pure operation must be added
/// here deliberately.
bool isMemoizableCombOperation(::mlir::Operation *op);

} // namespace pyc
