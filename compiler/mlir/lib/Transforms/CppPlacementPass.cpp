#include "pyc/Transforms/Passes.h"

#include "pyc/Emit/CppPlacement.h"

#include "mlir/Dialect/Func/IR/FuncOps.h"
#include "mlir/IR/BuiltinOps.h"
#include "mlir/Pass/Pass.h"

using namespace mlir;

namespace pyc {

struct CppPlacementPass : public PassWrapper<CppPlacementPass, OperationPass<ModuleOp>> {
  MLIR_DEFINE_EXPLICIT_INTERNAL_INLINE_TYPE_ID(CppPlacementPass)

  CppPlacementPass(unsigned chunkNodes, bool localize)
      : combChunkNodes(chunkNodes), localizeMembers(localize) {}

  StringRef getArgument() const override { return "pyc-cpp-placement"; }
  StringRef getDescription() const override {
    return "Set pyc.cpp.comb_chunk_nodes; optionally annotate comb member placement for C++ emit";
  }

  void runOnOperation() override {
    ModuleOp module = getOperation();
    if (combChunkNodes == 0) {
      module.emitError("pyc-cpp-placement requires combChunkNodes > 0");
      return signalPassFailure();
    }
    setModuleCombChunkNodes(module, combChunkNodes);

    if (!localizeMembers)
      return;

    for (auto f : module.getOps<func::FuncOp>()) {
      if (f.isDeclaration())
        continue;
      CppPlacementSummary summary = runCppMemberPlacement(f, combChunkNodes);
      setFuncPlacementSummary(f, summary);
    }
  }

  unsigned combChunkNodes;
  bool localizeMembers;
};

std::unique_ptr<Pass> createCppPlacementPass(unsigned combChunkNodes, bool localizeMembers) {
  return std::make_unique<CppPlacementPass>(combChunkNodes, localizeMembers);
}

} // namespace pyc
