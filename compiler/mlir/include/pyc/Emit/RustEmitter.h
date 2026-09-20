#pragma once

#include "mlir/Dialect/Func/IR/FuncOps.h"
#include "mlir/IR/BuiltinOps.h"
#include "mlir/Support/LogicalResult.h"
#include "llvm/Support/raw_ostream.h"

namespace pyc {

/// Experimental Rust functional-sim emitter (v1 subset).
/// Consumes the same post-gate IR as C++. Does not require pyc-cpp-placement.
::mlir::LogicalResult emitRust(::mlir::ModuleOp module, ::llvm::raw_ostream &os);

::mlir::LogicalResult emitRustFunc(::mlir::ModuleOp module, ::mlir::func::FuncOp f,
                                  ::llvm::raw_ostream &os);

} // namespace pyc
