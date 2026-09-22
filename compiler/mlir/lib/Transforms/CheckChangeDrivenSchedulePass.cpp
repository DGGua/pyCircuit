#include "pyc/Transforms/ChangeDrivenSchedule.h"

#include "pyc/Transforms/Passes.h"

#include "mlir/IR/BuiltinAttributes.h"
#include "mlir/IR/BuiltinOps.h"
#include "mlir/Pass/Pass.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/STLExtras.h"

using namespace mlir;

namespace pyc {
namespace {

static FailureOr<uint64_t> readUnsigned(Operation *op, StringRef name,
                                        unsigned resultIndex) {
  auto values = op->getAttrOfType<ArrayAttr>(name);
  if (!values || values.size() != op->getNumResults() ||
      resultIndex >= values.size()) {
    op->emitError("missing or invalid per-result schedule attribute '")
        << name << "'";
    return failure();
  }
  auto value = dyn_cast<IntegerAttr>(values[resultIndex]);
  if (!value || value.getInt() < 0) {
    op->emitError("schedule attribute contains a non-integer value '")
        << name << "'";
    return failure();
  }
  return static_cast<uint64_t>(value.getInt());
}

static LogicalResult verifyFanout(Operation *op,
                                  const ChangeScheduleNode &expected) {
  auto allFanouts = op->getAttrOfType<ArrayAttr>(kChangeScheduleFanoutAttr);
  if (!allFanouts || allFanouts.size() != op->getNumResults() ||
      expected.resultIndex >= allFanouts.size()) {
    op->emitError("missing per-result schedule fanout attribute");
    return failure();
  }
  auto attr = dyn_cast<ArrayAttr>(allFanouts[expected.resultIndex]);
  if (!attr) {
    op->emitError("schedule fanout entry is not an array");
    return failure();
  }
  if (attr.size() != expected.fanout.size()) {
    op->emitError("schedule fanout size mismatch");
    return failure();
  }
  for (auto indexed : llvm::enumerate(attr)) {
    auto target = dyn_cast<IntegerAttr>(indexed.value());
    if (!target || target.getInt() < 0 ||
        static_cast<uint64_t>(target.getInt()) !=
            expected.fanout[indexed.index()]) {
      op->emitError("schedule fanout does not match canonical graph");
      return failure();
    }
    if (static_cast<uint64_t>(target.getInt()) <= expected.id) {
      op->emitError("schedule fanout edge is not forward");
      return failure();
    }
  }
  return success();
}

static LogicalResult verifySummary(func::FuncOp func,
                                   const ChangeSchedulePlan &plan) {
  auto summary =
      func->getAttrOfType<DictionaryAttr>(kChangeScheduleSummaryAttr);
  if (!summary) {
    func.emitError("missing change-driven schedule summary");
    return failure();
  }
  auto schema = summary.getAs<StringAttr>("schema");
  if (!schema || schema.getValue() != kChangeScheduleSchema) {
    func.emitError("unsupported change-driven schedule schema");
    return failure();
  }
  auto nodeCount = summary.getAs<IntegerAttr>("node_count");
  auto edgeCount = summary.getAs<IntegerAttr>("edge_count");
  auto rankCount = summary.getAs<IntegerAttr>("rank_count");
  if (!nodeCount || !edgeCount || !rankCount ||
      nodeCount.getInt() != static_cast<int64_t>(plan.nodes.size()) ||
      edgeCount.getInt() != static_cast<int64_t>(plan.edgeCount) ||
      rankCount.getInt() != static_cast<int64_t>(plan.rankCount)) {
    func.emitError("change-driven schedule summary does not match "
                   "canonical graph");
    return failure();
  }
  return success();
}

struct CheckChangeDrivenSchedulePass
    : public PassWrapper<CheckChangeDrivenSchedulePass,
                         OperationPass<ModuleOp>> {
  MLIR_DEFINE_EXPLICIT_INTERNAL_INLINE_TYPE_ID(CheckChangeDrivenSchedulePass)

  StringRef getArgument() const override {
    return "pyc-check-change-driven-schedule";
  }
  StringRef getDescription() const override {
    return "Strictly verify canonical change-driven schedule metadata";
  }

  void runOnOperation() override {
    bool failedAny = false;
    for (func::FuncOp func : getOperation().getOps<func::FuncOp>()) {
      if (func.isDeclaration())
        continue;
      auto plan = buildChangeDrivenSchedule(func);
      if (failed(plan)) {
        failedAny = true;
        continue;
      }
      if (failed(verifySummary(func, *plan))) {
        failedAny = true;
        continue;
      }

      llvm::DenseSet<Operation *> expectedOperations;
      for (const ChangeScheduleNode &expected : plan->nodes) {
        expectedOperations.insert(expected.operation);
        auto id = readUnsigned(expected.operation, kChangeScheduleNodeAttr,
                               expected.resultIndex);
        auto rank = readUnsigned(expected.operation, kChangeScheduleRankAttr,
                                 expected.resultIndex);
        auto slot = readUnsigned(expected.operation, kChangeScheduleSlotAttr,
                                 expected.resultIndex);
        if (failed(id) || failed(rank) || failed(slot) || *id != expected.id ||
            *rank != expected.rank || *slot != expected.slot ||
            failed(verifyFanout(expected.operation, expected))) {
          failedAny = true;
          break;
        }
      }
      if (failedAny)
        break;

      func.walk([&](Operation *op) {
        if (op == func.getOperation() || expectedOperations.contains(op))
          return;
        if (op->hasAttr(kChangeScheduleNodeAttr) ||
            op->hasAttr(kChangeScheduleRankAttr) ||
            op->hasAttr(kChangeScheduleSlotAttr) ||
            op->hasAttr(kChangeScheduleFanoutAttr)) {
          op->emitError(
              "schedule metadata attached to unsupported/non-node operation");
          failedAny = true;
        }
      });
      if (failedAny)
        break;
    }
    if (failedAny)
      signalPassFailure();
  }
};

} // namespace

std::unique_ptr<::mlir::Pass> createCheckChangeDrivenSchedulePass() {
  return std::make_unique<CheckChangeDrivenSchedulePass>();
}

static PassRegistration<CheckChangeDrivenSchedulePass> checkPass;

} // namespace pyc
