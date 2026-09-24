#pragma once

#include "mlir/Dialect/Func/IR/FuncOps.h"
#include "mlir/IR/BuiltinOps.h"
#include "mlir/Support/LogicalResult.h"
#include "llvm/Support/raw_ostream.h"
#include "pyc/Simulation/SimulationPlan.h"

#include <string>

namespace pyc {

struct CppEmitterOptions {
  std::string probePlanPath{};
};

::mlir::LogicalResult emitCpp(const ModuleSimulationPlan &plan, ::llvm::raw_ostream &os,
                              const CppEmitterOptions &opts = {});

::mlir::LogicalResult emitCppFunc(const SimulationPlan &plan, ::llvm::raw_ostream &os,
                                  const CppEmitterOptions &opts = {});

} // namespace pyc
