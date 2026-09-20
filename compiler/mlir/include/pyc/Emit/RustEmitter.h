#pragma once

#include "mlir/Dialect/Func/IR/FuncOps.h"
#include "mlir/IR/BuiltinOps.h"
#include "mlir/Support/LogicalResult.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/raw_ostream.h"

#include <string>

namespace pyc {

/// Experimental Rust functional-sim emitter (v1 subset).
/// Consumes the same post-gate IR as C++. Does not require pyc-cpp-placement.
::mlir::LogicalResult emitRust(::mlir::ModuleOp module, ::llvm::raw_ostream &os);

::mlir::LogicalResult emitRustFunc(::mlir::ModuleOp module, ::mlir::func::FuncOp f,
                                  ::llvm::raw_ostream &os);

/// Sanitize an arbitrary symbol name into a legal Rust identifier that matches
/// the emitter's internal rules (non-alphanumerics rewritten, leading digits
/// prefixed, Rust keywords escaped). Callers that produce Rust source referring
/// to emitter-generated modules must use this to stay in sync.
std::string sanitizeRustIdent(::llvm::StringRef s);

} // namespace pyc
