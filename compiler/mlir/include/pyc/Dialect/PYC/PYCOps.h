#pragma once

#include "mlir/Bytecode/BytecodeOpInterface.h"
#include "mlir/IR/OpDefinition.h"
#include "mlir/Interfaces/InferTypeOpInterface.h"
#include "mlir/Interfaces/SideEffectInterfaces.h"

#include "pyc/Dialect/PYC/PYCDialect.h"
#include "pyc/Dialect/PYC/PYCTypes.h"

#define GET_OP_CLASSES
#include "pyc/Dialect/PYC/PYCOps.h.inc"

namespace pyc {
// Explicit debug retention is an observation effect, never a hardware state
// boundary. A name alone remains a codegen hint.
bool isDebugObservation(::mlir::Operation *op);
bool isHardwarePure(::mlir::Operation *op);
} // namespace pyc
