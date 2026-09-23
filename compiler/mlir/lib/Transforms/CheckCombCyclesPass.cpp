#include "pyc/Transforms/CombDepGraph.h"
#include "pyc/Transforms/Passes.h"

#include "pyc/Dialect/PYC/PYCOps.h"

#include "mlir/Dialect/Func/IR/FuncOps.h"
#include "mlir/IR/BuiltinOps.h"
#include "mlir/Pass/Pass.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/Twine.h"
#include "llvm/Support/raw_ostream.h"

using namespace mlir;

namespace pyc {
namespace {

static std::string valueLabel(Value v) {
  if (auto w = v.getDefiningOp<pyc::WireOp>()) {
    if (auto n = w->getAttrOfType<StringAttr>("pyc.name"))
      return n.getValue().str();
  }
  if (auto result = dyn_cast<OpResult>(v)) {
    if (auto instance = dyn_cast_or_null<pyc::InstanceOp>(result.getOwner())) {
      if (auto name = instance.getNameAttr())
        return (name.getValue() + ".result#" +
                Twine(result.getResultNumber()))
            .str();
    }
  }
  std::string s;
  llvm::raw_string_ostream os(s);
  v.print(os);
  os.flush();
  return s;
}

struct CheckCombCyclesPass : public PassWrapper<CheckCombCyclesPass, OperationPass<ModuleOp>> {
  MLIR_DEFINE_EXPLICIT_INTERNAL_INLINE_TYPE_ID(CheckCombCyclesPass)

  StringRef getArgument() const override { return "pyc-check-comb-cycles"; }
  StringRef getDescription() const override {
    return "Detect cycles in the canonical, instance-aware same-TICK "
           "combinational dependency graph";
  }

  void runOnOperation() override {
    ModuleOp module = getOperation();
    CombDepGraphCache combCache(module);

    bool failedAny = false;

    for (func::FuncOp f : module.getOps<func::FuncOp>()) {
      if (f.isDeclaration() || f.getBody().empty())
        continue;

      auto graph = FunctionCombDepGraph::build(f, combCache);
      if (failed(graph)) {
        f.emitError("failed to build canonical combinational dependency graph");
        failedAny = true;
        break;
      }

      llvm::SmallVector<unsigned> cycle;
      if (succeeded((*graph)->stableTopologicalOrder(&cycle)))
        continue;

      std::string message;
      llvm::raw_string_ostream os(message);
      // Preserve this diagnostic prefix: existing gates and downstream tooling
      // intentionally key off it.
      os << "combinational cycle detected: ";
      if (cycle.empty()) {
        os << "<cycle witness unavailable>";
      } else {
        ArrayRef<CombDepValueNode> nodes = (*graph)->getNodes();
        ArrayRef<CombDepEdge> edges = (*graph)->getEdges();
        for (auto [index, nodeId] : llvm::enumerate(cycle)) {
          if (nodeId < nodes.size())
            os << valueLabel(nodes[nodeId].value);
          else
            os << "<invalid-node#" << nodeId << ">";
          if (index + 1 >= cycle.size())
            continue;
          if (nodeId >= nodes.size()) {
            os << " -> ";
            continue;
          }

          const CombDepEdge *witnessEdge = nullptr;
          for (unsigned edgeId : nodes[nodeId].outgoingEdges) {
            if (edges[edgeId].target == cycle[index + 1]) {
              witnessEdge = &edges[edgeId];
              break;
            }
          }
          auto instance =
              witnessEdge && witnessEdge->kind == CombDepEdgeKind::InstancePort
                  ? dyn_cast_or_null<pyc::InstanceOp>(witnessEdge->owner)
                  : pyc::InstanceOp();
          if (!instance) {
            os << " -> ";
            continue;
          }
          StringRef instanceName =
              instance.getNameAttr() ? instance.getNameAttr().getValue()
                                     : StringRef("<anonymous>");
          StringRef callee = instance.getCalleeAttr().getValue();
          unsigned resultIndex =
              cycle[index + 1] < nodes.size()
                  ? nodes[cycle[index + 1]].resultIndex
                  : 0u;
          os << " -[instance " << instanceName << " @" << callee
             << " input#" << witnessEdge->operandIndex << " -> result#"
             << resultIndex << "]-> ";
        }
      }
      os.flush();
      f.emitError(message);
      failedAny = true;
      break;
    }

    if (failedAny)
      signalPassFailure();
  }
};

} // namespace

std::unique_ptr<::mlir::Pass> createCheckCombCyclesPass() { return std::make_unique<CheckCombCyclesPass>(); }

static PassRegistration<CheckCombCyclesPass> pass;

} // namespace pyc
