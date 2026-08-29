#include "pyc/Dialect/PYC/PYCDialect.h"
#include "pyc/Dialect/PYC/PYCOps.h"
#include "pyc/Transforms/CombPartition.h"
#include "pyc/Transforms/Passes.h"

#include "mlir/Dialect/Arith/IR/Arith.h"
#include "mlir/Dialect/Func/IR/FuncOps.h"
#include "mlir/IR/BuiltinOps.h"
#include "mlir/IR/MLIRContext.h"
#include "mlir/Parser/Parser.h"
#include "mlir/Pass/PassManager.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/Support/raw_ostream.h"

#include <string>

using namespace mlir;

static constexpr llvm::StringLiteral input = R"mlir(
module {
  func.func @local_partition(%a: i8, %b: i8) -> (i8, i8, i8) {
    %chain = pyc.comb(%a, %b) {pyc.name = "chain_out"} : (i8, i8) -> i8 {
    ^bb0(%x: i8, %y: i8):
      %0 = pyc.not %x : i8
      %1 = pyc.xor %0, %y : i8, i8 -> i8
      %2 = pyc.and %1, %x : i8, i8 -> i8
      %3 = pyc.or %2, %y : i8, i8 -> i8
      %4 = pyc.not %3 : i8
      pyc.yield %4 : i8
    } loc("local_partition.mlir":7:5)
    %dup:2 = pyc.comb(%b) {pyc.comb.result_names = ["tap_a", "tap_b"]} : (i8) -> (i8, i8) {
    ^bb0(%x: i8):
      %0 = pyc.not %x : i8
      pyc.yield %0, %0 : i8, i8
    }
    return %chain, %dup#0, %dup#1 : i8, i8, i8
  }
}
)mlir";

static FailureOr<std::string> runPartition(MLIRContext &context) {
  OwningOpRef<ModuleOp> module = parseSourceString<ModuleOp>(input, &context);
  if (!module)
    return failure();

  auto function = *module->getOps<func::FuncOp>().begin();
  Location chainLocation =
      (*function.getBody().front().getOps<pyc::CombOp>().begin()).getLoc();
  PassManager manager(&context);
  manager.addNestedPass<func::FuncOp>(
      pyc::createPartitionFusedCombPass(/*maxNodes=*/2));
  manager.addNestedPass<func::FuncOp>(pyc::createCheckCombPartitionsPass());
  if (failed(manager.run(*module)))
    return failure();

  llvm::SmallVector<pyc::CombOp> parts;
  for (pyc::CombOp comb : function.getBody().front().getOps<pyc::CombOp>())
    parts.push_back(comb);
  if (parts.size() != 4)
    return failure();

  unsigned totalWork = 0;
  unsigned chainNameCount = 0;
  unsigned tapNameCount = 0;
  for (pyc::CombOp comb : parts) {
    auto version =
        comb->getAttrOfType<StringAttr>(pyc::kCombPartitionPlanVersionAttr);
    if (!version) {
      auto names = comb->getAttrOfType<ArrayAttr>(pyc::kCombResultNamesAttr);
      if (!names || names.size() != 2)
        return failure();
      for (Attribute attribute : names) {
        auto name = dyn_cast<StringAttr>(attribute);
        if (!name)
          return failure();
        if (name.getValue() == "tap_a" || name.getValue() == "tap_b")
          ++tapNameCount;
      }
      continue;
    }
    auto work = comb->getAttrOfType<IntegerAttr>(pyc::kCombPartitionWorkAttr);
    auto parent =
        comb->getAttrOfType<IntegerAttr>(pyc::kCombPartitionParentIdAttr);
    if (version.getValue() != pyc::kFusedCombPartitionPlanVersion || !work ||
        work.getInt() < 0 || work.getInt() > 2 || !parent ||
        (parent.getInt() == 0 && comb.getLoc() != chainLocation))
      return failure();
    totalWork += static_cast<unsigned>(work.getInt());
    if (comb->getAttrOfType<StringAttr>("pyc.name"))
      ++chainNameCount;
    if (auto names =
            comb->getAttrOfType<ArrayAttr>(pyc::kCombResultNamesAttr)) {
      for (Attribute attribute : names) {
        auto name = dyn_cast<StringAttr>(attribute);
        if (!name)
          return failure();
        if (name.getValue() == "chain_out")
          ++chainNameCount;
        if (name.getValue() == "tap_a" || name.getValue() == "tap_b")
          ++tapNameCount;
      }
    }
  }
  if (totalWork != 5 || chainNameCount != 2 || tapNameCount != 2)
    return failure();

  // Generated local-fused-v1 combs are immutable to this pass.
  if (failed(manager.run(*module)))
    return failure();
  unsigned repeatedCount = 0;
  for (pyc::CombOp unused : function.getBody().front().getOps<pyc::CombOp>()) {
    (void)unused;
    ++repeatedCount;
  }
  if (repeatedCount != parts.size())
    return failure();

  std::string text;
  llvm::raw_string_ostream stream(text);
  module->print(stream);
  return text;
}

static bool rejectsNonMemoizableOperation(MLIRContext &context) {
  constexpr llvm::StringLiteral invalid = R"mlir(
module {
  func.func @invalid(%a: i8) -> i8 {
    %0 = pyc.comb(%a) : (i8) -> i8 {
    ^bb0(%x: i8):
      %1 = arith.addi %x, %x : i8
      pyc.yield %1 : i8
    }
    return %0 : i8
  }
}
)mlir";
  OwningOpRef<ModuleOp> module = parseSourceString<ModuleOp>(invalid, &context);
  if (!module)
    return false;
  PassManager manager(&context);
  manager.addNestedPass<func::FuncOp>(
      pyc::createPartitionFusedCombPass(/*maxNodes=*/2));
  return failed(manager.run(*module));
}

int main() {
  DialectRegistry registry;
  registry.insert<pyc::PYCDialect, arith::ArithDialect, func::FuncDialect>();
  MLIRContext context(registry);

  auto first = runPartition(context);
  auto second = runPartition(context);
  if (failed(first) || failed(second) || *first != *second ||
      !rejectsNonMemoizableOperation(context)) {
    llvm::errs() << "local fused-comb partition test failed\n";
    return 1;
  }
  llvm::outs() << "local fused-comb partition test passed\n";
  return 0;
}
