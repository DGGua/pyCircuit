#include "pyc/Transforms/Passes.h"
#include "pyc/Transforms/StateOptimization.h"

#include "pyc/Dialect/PYC/PYCOps.h"

#include "mlir/Dialect/Func/IR/FuncOps.h"
#include "mlir/IR/Builders.h"
#include "mlir/IR/BuiltinTypes.h"
#include "mlir/Pass/Pass.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/Hashing.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/STLExtras.h"

#include <algorithm>
#include <cstddef>

using namespace mlir;

namespace pyc {
namespace {

constexpr llvm::StringLiteral kOptimizedByAttr = "pyc.optimized_by";
constexpr llvm::StringLiteral kPackStateLanes = "pack_state_lanes";

struct StateLane {
  Operation *op = nullptr;
  Value q;
  Value clk;
  Value rst;
  Value en;
  Value next;
  Value init;
  unsigned width = 0;
  unsigned position = 0;
  int64_t depth = 1;
  bool isDelay = false;
  StringAttr stableName;
  llvm::SmallVector<pyc::AliasOp> aliases;
};

struct PackStats {
  int64_t groups = 0;
  int64_t regGroups = 0;
  int64_t delayGroups = 0;
  int64_t packedStateOps = 0;
  int64_t primitivesRemoved = 0;
  int64_t packedBits = 0;
};

static void setI64Attr(Operation *op, llvm::StringRef name, int64_t value) {
  OpBuilder builder(op->getContext());
  op->setAttr(name, builder.getI64IntegerAttr(value));
}

static int64_t getI64Attr(Operation *op, llvm::StringRef name,
                          int64_t fallback = 0) {
  if (auto attr = op->getAttrOfType<IntegerAttr>(name))
    return attr.getInt();
  return fallback;
}

static std::size_t valueHash(Value value) {
  value = stripStateAliases(value);
  if (auto constant = value.getDefiningOp<pyc::ConstantOp>()) {
    Attribute literal = constant->getAttr("value");
    return static_cast<std::size_t>(llvm::hash_combine(
        value.getType().getAsOpaquePointer(), literal.getAsOpaquePointer(), 1));
  }
  return static_cast<std::size_t>(llvm::hash_combine(
      value.getType().getAsOpaquePointer(), value.getAsOpaquePointer(), 0));
}

static std::size_t controlHash(const StateLane &lane) {
  return static_cast<std::size_t>(llvm::hash_combine(
      lane.isDelay, lane.depth, valueHash(lane.clk), valueHash(lane.rst),
      valueHash(lane.en)));
}

static bool sameControlKey(const StateLane &lhs, const StateLane &rhs) {
  return lhs.isDelay == rhs.isDelay && lhs.depth == rhs.depth &&
         equivalentStateValue(lhs.clk, rhs.clk) &&
         equivalentStateValue(lhs.rst, rhs.rst) &&
         equivalentStateValue(lhs.en, rhs.en);
}

static Value currentClock(const StateLane &lane) {
  if (auto reg = dyn_cast<pyc::RegOp>(lane.op))
    return reg.getClk();
  return cast<pyc::DelayLineOp>(lane.op).getClk();
}

static Value currentReset(const StateLane &lane) {
  if (auto reg = dyn_cast<pyc::RegOp>(lane.op))
    return reg.getRst();
  return cast<pyc::DelayLineOp>(lane.op).getRst();
}

static Value currentEnable(const StateLane &lane) {
  if (auto reg = dyn_cast<pyc::RegOp>(lane.op))
    return reg.getEn();
  return cast<pyc::DelayLineOp>(lane.op).getEn();
}

static Value currentNext(const StateLane &lane) {
  if (auto reg = dyn_cast<pyc::RegOp>(lane.op))
    return reg.getNext();
  return cast<pyc::DelayLineOp>(lane.op).getNext();
}

static Value currentInit(const StateLane &lane) {
  if (auto reg = dyn_cast<pyc::RegOp>(lane.op))
    return reg.getInit();
  return cast<pyc::DelayLineOp>(lane.op).getInit();
}

static void collectAliases(Value value,
                           llvm::SmallVectorImpl<pyc::AliasOp> &aliases,
                           llvm::DenseSet<Operation *> &seen) {
  for (Operation *user : value.getUsers()) {
    auto alias = dyn_cast<pyc::AliasOp>(user);
    if (!alias || !seen.insert(user).second)
      continue;
    aliases.push_back(alias);
    collectAliases(alias.getResult(), aliases, seen);
  }
}

static bool groupUsesAfter(ArrayRef<StateLane> lanes, unsigned insertPosition,
                           const llvm::DenseMap<Operation *, unsigned> &positions) {
  llvm::DenseSet<Operation *> stateOps;
  llvm::DenseSet<Operation *> aliases;
  for (const StateLane &lane : lanes) {
    stateOps.insert(lane.op);
    for (pyc::AliasOp alias : lane.aliases)
      aliases.insert(alias.getOperation());
  }

  auto checkValue = [&](Value value) {
    for (Operation *user : value.getUsers()) {
      if (aliases.contains(user))
        continue;
      if (stateOps.contains(user))
        return false;
      auto it = positions.find(user);
      if (it == positions.end() || it->second <= insertPosition)
        return false;
    }
    return true;
  };

  for (const StateLane &lane : lanes) {
    if (!checkValue(lane.q))
      return false;
    for (pyc::AliasOp alias : lane.aliases) {
      if (!checkValue(alias.getResult()))
        return false;
    }
  }
  return true;
}

static void replaceLaneUses(const StateLane &lane, Value replacement) {
  llvm::DenseSet<Operation *> aliases;
  for (pyc::AliasOp alias : lane.aliases)
    aliases.insert(alias.getOperation());

  Value q = lane.q;
  q.replaceUsesWithIf(replacement, [&](OpOperand &use) {
    return !aliases.contains(use.getOwner());
  });
  for (pyc::AliasOp alias : lane.aliases) {
    alias.getResult().replaceUsesWithIf(replacement, [&](OpOperand &use) {
      return !aliases.contains(use.getOwner());
    });
  }

  llvm::SmallVector<pyc::AliasOp> ordered(lane.aliases.begin(),
                                          lane.aliases.end());
  llvm::sort(ordered, [](pyc::AliasOp lhs, pyc::AliasOp rhs) {
    return lhs->isBeforeInBlock(rhs);
  });
  for (pyc::AliasOp alias : llvm::reverse(ordered))
    alias.erase();
}

static void packGroup(ArrayRef<StateLane> lanes, PackStats &stats) {
  if (lanes.size() < 2)
    return;

  const StateLane *last = &lanes.front();
  unsigned packedWidth = 0;
  bool allGenerated = true;
  int64_t sourceRegs = 0;
  int64_t sourceChains = 0;
  for (const StateLane &lane : lanes) {
    packedWidth += lane.width;
    if (lane.position > last->position)
      last = &lane;
    allGenerated &= isCycleBalanceGenerated(lane.op);
    sourceRegs += getI64Attr(lane.op, "pyc.source_reg_count", 0);
    sourceChains += getI64Attr(lane.op, "pyc.shared_chain_count", 0);
  }

  OpBuilder builder(last->op);
  Type packedType = builder.getIntegerType(packedWidth);
  llvm::SmallVector<Value> nextInputs;
  llvm::SmallVector<Value> initInputs;
  nextInputs.reserve(lanes.size());
  initInputs.reserve(lanes.size());
  for (const StateLane &lane : llvm::reverse(lanes)) {
    nextInputs.push_back(currentNext(lane));
    initInputs.push_back(currentInit(lane));
  }

  Location loc = last->op->getLoc();
  Value packedNext =
      builder.create<pyc::ConcatOp>(loc, packedType, nextInputs).getResult();
  Value packedInit =
      builder.create<pyc::ConcatOp>(loc, packedType, initInputs).getResult();
  Operation *packedOp = nullptr;
  Value packedQ;
  if (last->isDelay) {
    auto delay = builder.create<pyc::DelayLineOp>(
        loc, packedType, currentClock(*last), currentReset(*last),
        currentEnable(*last), packedNext, packedInit);
    delay->setAttr("depth", builder.getI64IntegerAttr(last->depth));
    packedOp = delay.getOperation();
    packedQ = delay.getQ();
  } else {
    auto reg = builder.create<pyc::RegOp>(
        loc, packedType, currentClock(*last), currentReset(*last),
        currentEnable(*last), packedNext, packedInit);
    packedOp = reg.getOperation();
    packedQ = reg.getQ();
  }

  packedOp->setAttr(kOptimizedByAttr, builder.getStringAttr(kPackStateLanes));
  packedOp->setAttr("pyc.state_pack_lanes",
                    builder.getI64IntegerAttr(lanes.size()));
  packedOp->setAttr("pyc.state_pack_width",
                    builder.getI64IntegerAttr(packedWidth));
  if (allGenerated)
    packedOp->setAttr("pyc.generated",
                      builder.getStringAttr("cycle_balance"));
  if (sourceRegs)
    packedOp->setAttr("pyc.source_reg_count",
                      builder.getI64IntegerAttr(sourceRegs));
  if (sourceChains)
    packedOp->setAttr("pyc.shared_chain_count",
                      builder.getI64IntegerAttr(sourceChains));

  unsigned lsb = 0;
  llvm::SmallVector<Value> replacements;
  replacements.reserve(lanes.size());
  for (const StateLane &lane : lanes) {
    Type laneType = builder.getIntegerType(lane.width);
    auto extract = builder.create<pyc::ExtractOp>(
        lane.op->getLoc(), laneType, packedQ,
        builder.getI64IntegerAttr(lsb),
        builder.getI64IntegerAttr(lsb + lane.width - 1));
    extract->setAttr("pyc.state_pack_lsb", builder.getI64IntegerAttr(lsb));
    extract->setAttr("pyc.state_pack_source_width",
                     builder.getI64IntegerAttr(packedWidth));
    Value replacement = extract.getResult();
    if (lane.stableName) {
      auto alias = builder.create<pyc::AliasOp>(lane.op->getLoc(), laneType,
                                                replacement);
      alias->setAttr("pyc.name", lane.stableName);
      replacement = alias.getResult();
    }
    replacements.push_back(replacement);
    lsb += lane.width;
  }

  for (auto [lane, replacement] : llvm::zip(lanes, replacements))
    replaceLaneUses(lane, replacement);
  for (const StateLane &lane : llvm::reverse(lanes))
    lane.op->erase();

  ++stats.groups;
  stats.regGroups += last->isDelay ? 0 : 1;
  stats.delayGroups += last->isDelay ? 1 : 0;
  stats.packedStateOps += static_cast<int64_t>(lanes.size());
  stats.primitivesRemoved += static_cast<int64_t>(lanes.size()) - 1;
  stats.packedBits +=
      static_cast<int64_t>(packedWidth) * static_cast<int64_t>(last->depth);
}

class PackStateLanesPass
    : public PassWrapper<PackStateLanesPass, OperationPass<func::FuncOp>> {
public:
  MLIR_DEFINE_EXPLICIT_INTERNAL_INLINE_TYPE_ID(PackStateLanesPass)

  PackStateLanesPass() = default;
  PackStateLanesPass(const PackStateLanesPass &other) : PassWrapper(other) {}
  PackStateLanesPass(unsigned maxWidth, bool preserveObservability) {
    maxWidthOption = maxWidth;
    preserveObservabilityOption = preserveObservability;
  }

  StringRef getArgument() const override { return "pyc-pack-state-lanes"; }
  StringRef getDescription() const override {
    return "Pack compatible integer state lanes into wider storage";
  }

  Option<unsigned> maxWidthOption{
      *this, "max-width",
      llvm::cl::desc("Maximum packed integer width (0 disables packing)"),
      llvm::cl::init(192)};
  Option<bool> preserveObservabilityOption{
      *this, "preserve-observability",
      llvm::cl::desc("Keep named/debug/probe/trace state identities"),
      llvm::cl::init(false)};

  void runOnOperation() override {
    func::FuncOp function = getOperation();
    PackStats stats;
    if (maxWidthOption != 0) {
      StateObservabilityAnalysis observability(
          function, preserveObservabilityOption);
      for (Block &block : function.getBody())
        packBlock(block, observability, stats);
    }

    setI64Attr(function, "pyc.stats.state_opt_pack_groups", stats.groups);
    setI64Attr(function, "pyc.stats.state_opt_pack_reg_groups",
               stats.regGroups);
    setI64Attr(function, "pyc.stats.state_opt_pack_delay_groups",
               stats.delayGroups);
    setI64Attr(function, "pyc.stats.state_opt_packed_state_ops",
               stats.packedStateOps);
    setI64Attr(function, "pyc.stats.state_opt_state_primitives_removed",
               stats.primitivesRemoved);
    setI64Attr(function, "pyc.stats.state_opt_pack_bits", stats.packedBits);
  }

private:
  void packBlock(Block &block,
                 const StateObservabilityAnalysis &observability,
                 PackStats &stats) {
    llvm::DenseMap<Operation *, unsigned> positions;
    unsigned nextPosition = 0;
    for (Operation &op : block)
      positions[&op] = nextPosition++;

    llvm::DenseMap<std::size_t, llvm::SmallVector<unsigned>> bucketIndexes;
    llvm::SmallVector<llvm::SmallVector<StateLane>> exactBuckets;
    for (Operation &op : block) {
      StateLane lane;
      if (auto reg = dyn_cast<pyc::RegOp>(op)) {
        lane.op = &op;
        lane.q = reg.getQ();
        lane.clk = reg.getClk();
        lane.rst = reg.getRst();
        lane.en = reg.getEn();
        lane.next = reg.getNext();
        lane.init = reg.getInit();
      } else if (auto delay = dyn_cast<pyc::DelayLineOp>(op)) {
        auto depth = delay->getAttrOfType<IntegerAttr>("depth");
        if (!depth)
          continue;
        lane.op = &op;
        lane.q = delay.getQ();
        lane.clk = delay.getClk();
        lane.rst = delay.getRst();
        lane.en = delay.getEn();
        lane.next = delay.getNext();
        lane.init = delay.getInit();
        lane.depth = depth.getInt();
        lane.isDelay = true;
      } else {
        continue;
      }

      auto type = dyn_cast<IntegerType>(lane.q.getType());
      if (!type || type.getWidth() == 0 || type.getWidth() > maxWidthOption)
        continue;
      lane.width = type.getWidth();
      lane.position = positions.lookup(lane.op);
      llvm::DenseSet<Operation *> aliases;
      collectAliases(lane.q, lane.aliases, aliases);
      if (preserveObservabilityOption && observability.isPinned(lane.op)) {
        // A stable logical name can survive packing on a slice alias. Stronger
        // observation/debug attributes and named aliases retain physical state.
        if (!hasStableStateName(lane.op) ||
            shouldKeepStateOptimization(lane.op) ||
            llvm::any_of(lane.aliases, [](pyc::AliasOp alias) {
              return hasStableStateName(alias) ||
                     shouldKeepStateOptimization(alias);
            }))
          continue;
        lane.stableName = lane.op->getAttrOfType<StringAttr>("pyc.name");
      }
      auto &candidateIndexes = bucketIndexes[controlHash(lane)];
      auto match = llvm::find_if(candidateIndexes, [&](unsigned index) {
        return sameControlKey(exactBuckets[index].front(), lane);
      });
      if (match == candidateIndexes.end()) {
        candidateIndexes.push_back(exactBuckets.size());
        exactBuckets.push_back({std::move(lane)});
      } else {
        exactBuckets[*match].push_back(std::move(lane));
      }
    }

    // exactBuckets is created in block order; the DenseMap is lookup-only so
    // hash iteration can never perturb generated IR ordering.
    for (auto &bucket : exactBuckets) {
      llvm::SmallVector<StateLane> group;
      unsigned groupWidth = 0;
      auto flush = [&]() {
        if (group.size() >= 2) {
          unsigned insertPosition = group.back().position;
          if (groupUsesAfter(group, insertPosition, positions))
            packGroup(group, stats);
        }
        group.clear();
        groupWidth = 0;
      };

      for (const StateLane &lane : bucket) {
        if (!group.empty() && groupWidth + lane.width > maxWidthOption)
          flush();
        llvm::SmallVector<StateLane> prospective(group.begin(), group.end());
        prospective.push_back(lane);
        if (!group.empty() &&
            !groupUsesAfter(prospective, lane.position, positions))
          flush();
        group.push_back(lane);
        groupWidth += lane.width;
      }
      flush();
    }
  }
};

} // namespace

std::unique_ptr<::mlir::Pass>
createPackStateLanesPass(unsigned maxWidth, bool preserveObservability) {
  return std::make_unique<PackStateLanesPass>(maxWidth,
                                               preserveObservability);
}

static PassRegistration<PackStateLanesPass> pass;

} // namespace pyc
