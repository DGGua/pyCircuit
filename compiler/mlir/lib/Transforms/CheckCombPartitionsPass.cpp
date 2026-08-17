#include "pyc/Transforms/Passes.h"

#include "pyc/Dialect/PYC/PYCOps.h"

#include "mlir/Dialect/Func/IR/FuncOps.h"
#include "mlir/IR/BuiltinAttributes.h"
#include "mlir/Pass/Pass.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallVector.h"

#include <cstdint>
#include <limits>

using namespace mlir;

namespace pyc {
namespace {

constexpr llvm::StringLiteral kParentIdAttr = "pyc.partition.parent_id";
constexpr llvm::StringLiteral kPartIdAttr = "pyc.partition.part_id";
constexpr llvm::StringLiteral kPartCountAttr = "pyc.partition.part_count";
constexpr llvm::StringLiteral kPlanVersionAttr = "pyc.partition.plan_version";
constexpr llvm::StringLiteral kWorkAttr = "pyc.partition.work";
constexpr llvm::StringLiteral kMaxNodesAttr = "pyc.partition.max_nodes";
constexpr llvm::StringLiteral kPlanVersion = "gsim-contiguous-dp-v1";

static bool hasAnyPartitionAttribute(Operation *op) {
  return op->hasAttr(kParentIdAttr) || op->hasAttr(kPartIdAttr) ||
         op->hasAttr(kPartCountAttr) || op->hasAttr(kPlanVersionAttr) ||
         op->hasAttr(kWorkAttr) || op->hasAttr(kMaxNodesAttr);
}

static FailureOr<uint64_t> readUnsignedAttr(pyc::CombOp comb, StringRef name) {
  auto attr = comb->getAttrOfType<IntegerAttr>(name);
  if (!attr) {
    comb.emitOpError("partitioned comb requires integer attribute '")
        << name << "'";
    return failure();
  }
  int64_t value = attr.getInt();
  if (value < 0) {
    comb.emitOpError("partition attribute '")
        << name << "' must be non-negative";
    return failure();
  }
  return static_cast<uint64_t>(value);
}

static uint64_t bodyWork(pyc::CombOp comb) {
  uint64_t work = 0;
  for (Operation &op : comb.getBody().front().without_terminator())
    (void)op, ++work;
  return work;
}

struct PartitionInfo {
  pyc::CombOp comb;
  uint64_t parentId;
  uint64_t partId;
  uint64_t partCount;
  uint64_t work;
  uint64_t maxNodes;
};

struct CheckCombPartitionsPass
    : public PassWrapper<CheckCombPartitionsPass, OperationPass<func::FuncOp>> {
  MLIR_DEFINE_EXPLICIT_INTERNAL_INLINE_TYPE_ID(CheckCombPartitionsPass)

  StringRef getArgument() const override { return "pyc-check-comb-partitions"; }
  StringRef getDescription() const override {
    return "Verify sibling pyc.comb runtime partition plans";
  }

  void runOnOperation() override {
    func::FuncOp function = getOperation();
    llvm::DenseMap<uint64_t, llvm::SmallVector<PartitionInfo>> groups;
    bool anyFailure = false;

    function.walk([&](pyc::CombOp comb) {
      if (anyFailure || !hasAnyPartitionAttribute(comb))
        return;

      auto parentId = readUnsignedAttr(comb, kParentIdAttr);
      auto partId = readUnsignedAttr(comb, kPartIdAttr);
      auto partCount = readUnsignedAttr(comb, kPartCountAttr);
      auto work = readUnsignedAttr(comb, kWorkAttr);
      auto maxNodes = readUnsignedAttr(comb, kMaxNodesAttr);
      auto plan = comb->getAttrOfType<StringAttr>(kPlanVersionAttr);
      if (failed(parentId) || failed(partId) || failed(partCount) ||
          failed(work) || failed(maxNodes)) {
        anyFailure = true;
        return;
      }
      if (!plan || plan.getValue() != kPlanVersion) {
        comb.emitOpError("partition attribute '")
            << kPlanVersionAttr << "' must equal '" << kPlanVersion << "'";
        anyFailure = true;
        return;
      }
      if (*partCount == 0 || *partId >= *partCount) {
        comb.emitOpError(
            "partition part_id must be less than non-zero part_count");
        anyFailure = true;
        return;
      }
      if (*maxNodes == 0) {
        comb.emitOpError("partition max_nodes must be positive");
        anyFailure = true;
        return;
      }
      uint64_t actualWork = bodyWork(comb);
      if (*work != actualWork) {
        comb.emitOpError("partition work metadata mismatch: expected ")
            << actualWork << ", got " << *work;
        anyFailure = true;
        return;
      }
      if (*work > *maxNodes) {
        comb.emitOpError("partition work exceeds max_nodes: ")
            << *work << " > " << *maxNodes;
        anyFailure = true;
        return;
      }
      // A partition boundary must contain direct live-ins only. Completeness is
      // enforced by CombOp's IsolatedFromAbove verifier; reject redundant
      // operands here so transitive-closure inputs cannot silently inflate
      // snapshot work or spuriously activate a runtime partition.
      Block &body = comb.getBody().front();
      for (auto [index, argument] : llvm::enumerate(body.getArguments())) {
        if (!argument.use_empty())
          continue;
        comb.emitOpError("partition has redundant live-in operand ") << index;
        anyFailure = true;
        return;
      }
      groups[*parentId].push_back(PartitionInfo{comb, *parentId, *partId,
                                                *partCount, *work, *maxNodes});
    });
    if (anyFailure) {
      signalPassFailure();
      return;
    }

    for (auto &entry : groups) {
      auto &parts = entry.second;
      if (parts.empty())
        continue;
      uint64_t count = parts.front().partCount;
      uint64_t maxNodes = parts.front().maxNodes;
      if (parts.size() != count) {
        parts.front().comb.emitOpError("partition parent_id ")
            << entry.first << " declares " << count << " parts but has "
            << parts.size();
        anyFailure = true;
        continue;
      }

      llvm::SmallVector<pyc::CombOp> byId(count);
      for (PartitionInfo &part : parts) {
        if (part.partCount != count || part.maxNodes != maxNodes) {
          part.comb.emitOpError("partition siblings disagree on plan metadata");
          anyFailure = true;
          continue;
        }
        if (byId[part.partId]) {
          part.comb.emitOpError("duplicate partition part_id ") << part.partId;
          anyFailure = true;
          continue;
        }
        byId[part.partId] = part.comb;
      }
      if (anyFailure)
        continue;

      Block *parentBlock = byId.front()->getBlock();
      for (uint64_t i = 0; i < count; ++i) {
        pyc::CombOp comb = byId[i];
        if (!comb || comb->getBlock() != parentBlock) {
          parts.front().comb.emitOpError(
              "all sibling partitions must be present in one block");
          anyFailure = true;
          break;
        }
        if (i != 0 && byId[i - 1]->getNextNode() != comb.getOperation()) {
          comb.emitOpError(
              "sibling partitions must be adjacent and ordered by part_id");
          anyFailure = true;
          break;
        }

        // SSA dominance already rejects backward uses.  Check the explicit
        // partition contract as well so scheduler metadata cannot disagree
        // with the dependence graph it is meant to encode.
        for (Value input : comb.getInputs()) {
          auto producer = input.getDefiningOp<pyc::CombOp>();
          if (!producer)
            continue;
          auto producerParent =
              producer->getAttrOfType<IntegerAttr>(kParentIdAttr);
          if (!producerParent ||
              producerParent.getInt() != static_cast<int64_t>(entry.first))
            continue;
          auto producerPart = producer->getAttrOfType<IntegerAttr>(kPartIdAttr);
          if (!producerPart ||
              producerPart.getInt() >= static_cast<int64_t>(i)) {
            comb.emitOpError(
                "partition dependency must point strictly forward");
            anyFailure = true;
            break;
          }
        }
        if (anyFailure)
          break;
      }
    }

    if (anyFailure)
      signalPassFailure();
  }
};

} // namespace

std::unique_ptr<mlir::Pass> createCheckCombPartitionsPass() {
  return std::make_unique<CheckCombPartitionsPass>();
}

static PassRegistration<CheckCombPartitionsPass> pass;

} // namespace pyc
