#include "pyc/Transforms/Passes.h"
#include "pyc/Transforms/StateOptimization.h"

#include "mlir/Dialect/Func/IR/FuncOps.h"
#include "mlir/IR/BuiltinOps.h"
#include "mlir/IR/BuiltinTypes.h"
#include "mlir/Pass/Pass.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/ADT/StringSet.h"
#include "mlir/IR/BuiltinAttributes.h"
#include "llvm/Support/JSON.h"
#include "llvm/Support/MemoryBuffer.h"

#include <optional>
#include <string>

using namespace mlir;

namespace pyc {
namespace {

static void setI64Attr(Operation *op, StringRef name, int64_t value) {
  op->setAttr(name, IntegerAttr::get(IntegerType::get(op->getContext(), 64),
                                     value));
}

static constexpr StringRef kLazySlicesAttr = "pyc.lazy_probe_slices";
static constexpr StringRef kLazyWiresAttr = "pyc.lazy_probe_wires";

struct WireProbeHost {
  Operation *op = nullptr;
  unsigned resultIndex = 0;
};

struct StateProbeSource {
  Operation *storage = nullptr;
  unsigned lsb = 0;
  int64_t tapDepth = -1;
};

static unsigned valueWidth(Value value) {
  if (auto integer = dyn_cast<IntegerType>(value.getType()))
    return integer.getWidth();
  return 0;
}

static std::optional<StateProbeSource> findRegStorage(Value value) {
  llvm::SmallVector<Value, 8> seen;
  unsigned lsb = 0;
  while (true) {
    for (Value prev : seen) {
      if (prev == value)
        return std::nullopt;
    }
    seen.push_back(value);
    while (auto alias = value.getDefiningOp<pyc::AliasOp>())
      value = alias.getIn();
    if (auto reg = value.getDefiningOp<pyc::RegOp>())
      return StateProbeSource{reg.getOperation(), lsb};
    if (auto delay = value.getDefiningOp<pyc::DelayLineOp>())
      return StateProbeSource{delay.getOperation(), lsb};
    if (auto tap = value.getDefiningOp<pyc::DelayTapOp>()) {
      auto delay = tap.getLine().getDefiningOp<pyc::DelayLineOp>();
      auto depth = tap->getAttrOfType<IntegerAttr>("depth");
      if (!delay || !depth)
        return std::nullopt;
      return StateProbeSource{delay.getOperation(), lsb, depth.getInt()};
    }
    if (auto extract = value.getDefiningOp<pyc::ExtractOp>()) {
      auto packedLsb =
          extract->getAttrOfType<IntegerAttr>("pyc.state_pack_lsb");
      const int64_t sliceLsb =
          packedLsb ? packedLsb.getInt() : extract.getLsbAttr().getInt();
      if (sliceLsb < 0)
        return std::nullopt;
      lsb += static_cast<unsigned>(sliceLsb);
      value = extract.getIn();
      continue;
    }
    auto comb = value.getDefiningOp<pyc::CombOp>();
    if (!comb)
      return std::nullopt;
    auto res = dyn_cast<OpResult>(value);
    if (!res)
      return std::nullopt;
    auto yield =
        dyn_cast_or_null<pyc::YieldOp>(comb.getBody().front().getTerminator());
    if (!yield || res.getResultNumber() >= yield.getNumOperands())
      return std::nullopt;
    Value y = yield.getOperand(res.getResultNumber());
    while (auto alias = y.getDefiningOp<pyc::AliasOp>())
      y = alias.getIn();
    if (auto extract = y.getDefiningOp<pyc::ExtractOp>()) {
      auto packedLsb =
          extract->getAttrOfType<IntegerAttr>("pyc.state_pack_lsb");
      const int64_t sliceLsb =
          packedLsb ? packedLsb.getInt() : extract.getLsbAttr().getInt();
      if (sliceLsb < 0)
        return std::nullopt;
      lsb += static_cast<unsigned>(sliceLsb);
      y = extract.getIn();
      while (auto alias = y.getDefiningOp<pyc::AliasOp>())
        y = alias.getIn();
    }
    auto barg = dyn_cast<BlockArgument>(y);
    if (!barg || barg.getOwner() != &comb.getBody().front() ||
        barg.getArgNumber() >= comb.getNumOperands())
      return std::nullopt;
    value = comb.getOperand(barg.getArgNumber());
  }
}

static LogicalResult appendLazySlice(Operation *storage, StringRef name,
                                     unsigned lsb, unsigned width,
                                     int64_t tapDepth) {
  MLIRContext *ctx = storage->getContext();
  SmallVector<Attribute> items;
  if (auto existing = storage->getAttrOfType<ArrayAttr>(kLazySlicesAttr))
    items.append(existing.begin(), existing.end());
  for (Attribute item : items) {
    auto dict = dyn_cast<DictionaryAttr>(item);
    if (!dict)
      continue;
    if (auto prev = dict.getAs<StringAttr>("name");
        prev && prev.getValue() == name)
      return success();
  }
  NamedAttrList fields;
  fields.set("name", StringAttr::get(ctx, name));
  fields.set("lsb", IntegerAttr::get(IntegerType::get(ctx, 64), lsb));
  fields.set("width", IntegerAttr::get(IntegerType::get(ctx, 64), width));
  fields.set("tap_depth",
             IntegerAttr::get(IntegerType::get(ctx, 64), tapDepth));
  items.push_back(DictionaryAttr::get(ctx, fields));
  storage->setAttr(kLazySlicesAttr, ArrayAttr::get(ctx, items));
  return success();
}

static LogicalResult appendLazyWire(Operation *host, StringRef name,
                                    unsigned width, unsigned resultIndex) {
  MLIRContext *ctx = host->getContext();
  SmallVector<Attribute> items;
  if (auto existing = host->getAttrOfType<ArrayAttr>(kLazyWiresAttr))
    items.append(existing.begin(), existing.end());
  for (Attribute item : items) {
    auto dict = dyn_cast<DictionaryAttr>(item);
    if (!dict)
      continue;
    if (auto prev = dict.getAs<StringAttr>("name");
        prev && prev.getValue() == name)
      return success();
  }
  NamedAttrList fields;
  fields.set("name", StringAttr::get(ctx, name));
  fields.set("width", IntegerAttr::get(IntegerType::get(ctx, 64), width));
  fields.set("result",
             IntegerAttr::get(IntegerType::get(ctx, 64), resultIndex));
  items.push_back(DictionaryAttr::get(ctx, fields));
  host->setAttr(kLazyWiresAttr, ArrayAttr::get(ctx, items));
  return success();
}

// Point undeclared combinational names at an SSA that will be a C++ member
// after fuse/placement, so lookup does not copy a second Wire.
static std::optional<WireProbeHost> findWireHost(Value value) {
  while (auto alias = value.getDefiningOp<pyc::AliasOp>())
    value = alias.getIn();
  if (auto comb = value.getDefiningOp<pyc::CombOp>()) {
    auto res = dyn_cast<OpResult>(value);
    if (!res)
      return std::nullopt;
    return WireProbeHost{comb.getOperation(), res.getResultNumber()};
  }
  Operation *def = value.getDefiningOp();
  if (!def)
    return std::nullopt;
  if (isa<pyc::RegOp, pyc::DelayLineOp, pyc::DelayTapOp>(def))
    return std::nullopt;
  if (auto comb = def->getParentOfType<pyc::CombOp>()) {
    auto yield =
        dyn_cast_or_null<pyc::YieldOp>(comb.getBody().front().getTerminator());
    if (!yield)
      return std::nullopt;
    for (auto [i, operand] : llvm::enumerate(yield.getOperands())) {
      Value y = operand;
      while (auto alias = y.getDefiningOp<pyc::AliasOp>())
        y = alias.getIn();
      if (y == value)
        return WireProbeHost{comb.getOperation(),
                             static_cast<unsigned>(i)};
    }
    return std::nullopt;
  }
  auto res = dyn_cast<OpResult>(value);
  if (!res)
    return std::nullopt;
  return WireProbeHost{def, res.getResultNumber()};
}

static llvm::StringRef fieldPathFromProbePath(llvm::StringRef path) {
  const size_t colon = path.find(':');
  if (colon == llvm::StringRef::npos || colon + 1 >= path.size())
    return {};
  return path.substr(colon + 1);
}

static LogicalResult loadProbePlanFields(ModuleOp module, StringRef path,
                                         llvm::StringSet<> &fields) {
  if (path.empty())
    return success();
  auto fileOrErr = llvm::MemoryBuffer::getFile(path);
  if (!fileOrErr)
    return module.emitError("cannot read probe plan: ") << path;
  auto parsed = llvm::json::parse(fileOrErr.get()->getBuffer());
  if (!parsed || !parsed->getAsObject())
    return module.emitError("invalid probe plan JSON: ") << path;
  const auto *root = parsed->getAsObject();
  const auto *aliases = root->getArray("aliases");
  if (!aliases)
    return module.emitError("probe plan requires array `aliases`");
  for (const llvm::json::Value &value : *aliases) {
    const auto *entry = value.getAsObject();
    if (!entry)
      return module.emitError("probe plan alias must be an object");
    auto source = entry->getString("source_path");
    if (!source || source->empty())
      return module.emitError("probe plan alias requires non-empty source_path");
    const llvm::StringRef field = fieldPathFromProbePath(*source);
    if (field.empty())
      return module.emitError(
                 "probe plan source_path must be instance:field_path: ")
             << *source;
    fields.insert(field);
  }
  return success();
}

static LogicalResult loadTracePlanFields(ModuleOp module, StringRef path,
                                         llvm::StringMap<llvm::StringSet<>>
                                             &fieldsByModule) {
  if (path.empty())
    return success();
  auto fileOrErr = llvm::MemoryBuffer::getFile(path);
  if (!fileOrErr)
    return module.emitError("cannot read C++ trace codegen plan: ") << path;
  auto parsed = llvm::json::parse(fileOrErr.get()->getBuffer());
  if (!parsed || !parsed->getAsObject())
    return module.emitError("invalid C++ trace codegen plan JSON: ") << path;
  const auto *root = parsed->getAsObject();
  auto version = root->getInteger("version");
  const auto *traceModules = root->getObject("modules");
  if (!version || *version != 1 || !traceModules)
    return module.emitError(
        "C++ trace codegen plan requires version=1 and object `modules`");
  for (const auto &moduleEntry : *traceModules) {
    const auto *listed = moduleEntry.second.getAsArray();
    if (!listed)
      return module.emitError(
                 "C++ trace codegen module entry must be an array: ")
             << moduleEntry.first.str();
    llvm::StringSet<> &fields = fieldsByModule[moduleEntry.first];
    for (const llvm::json::Value &field : *listed) {
      auto name = field.getAsString();
      if (!name || name->empty())
        return module.emitError(
            "C++ trace codegen module fields must be non-empty strings");
      fields.insert(*name);
    }
  }
  return success();
}

// After merge/retime/pack remapped identities, mark undeclared pyc.name
// values as lazy lookups. Decision 0091/0096: those names stay findable
// but must not pay per-cycle extract/pin cost. IR attrs debug_keep /
// observable / probe* / trace* still count as eager demand.
struct ApplyObservationDemandPass
    : public PassWrapper<ApplyObservationDemandPass, OperationPass<ModuleOp>> {
  MLIR_DEFINE_EXPLICIT_INTERNAL_INLINE_TYPE_ID(ApplyObservationDemandPass)

  ApplyObservationDemandPass() = default;
  ApplyObservationDemandPass(const ApplyObservationDemandPass &other)
      : PassWrapper(other) {}
  ApplyObservationDemandPass(std::string probePlan,
                             std::string traceCodegenPlan) {
    probePlanOption = std::move(probePlan);
    tracePlanOption = std::move(traceCodegenPlan);
  }

  StringRef getArgument() const override {
    return "pyc-apply-observation-demand";
  }
  StringRef getDescription() const override {
    return "Mark undeclared pyc.name as lazy lookups after state opt";
  }

  Option<std::string> probePlanOption{
      *this, "probe-plan",
      llvm::cl::desc("Resolved @probe alias plan JSON (source_path fields)"),
      llvm::cl::init("")};
  Option<std::string> tracePlanOption{
      *this, "trace-codegen-plan",
      llvm::cl::desc("Per-module internal fields selected for C++ trace"),
      llvm::cl::init("")};

  void runOnOperation() override {
    ModuleOp module = getOperation();
    llvm::StringSet<> probeFields;
    llvm::StringMap<llvm::StringSet<>> traceFieldsByModule;
    if (failed(loadProbePlanFields(module, probePlanOption, probeFields)) ||
        failed(loadTracePlanFields(module, tracePlanOption,
                                   traceFieldsByModule))) {
      signalPassFailure();
      return;
    }

    for (auto f : module.getOps<func::FuncOp>()) {
      if (f.isDeclaration())
        continue;
      if (failed(applyToFunction(f, probeFields, traceFieldsByModule))) {
        signalPassFailure();
        return;
      }
    }
  }

  LogicalResult
  applyToFunction(func::FuncOp function, const llvm::StringSet<> &probeFields,
                  const llvm::StringMap<llvm::StringSet<>> &traceFieldsByModule) {
    llvm::StringSet<> demand = probeFields;
    llvm::StringSet<> presentBefore;
    function.walk([&](Operation *op) {
      if (auto name = op->getAttrOfType<StringAttr>("pyc.name"))
        presentBefore.insert(name.getValue());
    });

    if (const auto it = traceFieldsByModule.find(function.getSymName());
        it != traceFieldsByModule.end()) {
      for (const auto &field : it->second) {
        demand.insert(field.getKey());
        if (!presentBefore.contains(field.getKey()))
          return function.emitError(
                     "C++ trace codegen field not found in module: ")
                 << field.getKey();
      }
    }

    int64_t lazy = 0;
    int64_t kept = 0;
    auto mark = [&](Operation *op) -> LogicalResult {
      auto name = op->getAttrOfType<StringAttr>("pyc.name");
      if (!name)
        return success();
      if (isCycleBalanceGenerated(op))
        return success();
      const bool demanded = shouldKeepStateOptimization(op) ||
                            demand.contains(name.getValue());
      if (demanded) {
        op->removeAttr("pyc.observe_lazy");
        ++kept;
        return success();
      }
      if (op->getNumResults() != 1) {
        ++kept;
        return success();
      }
      const unsigned width = valueWidth(op->getResult(0));
      if (width == 0) {
        ++kept;
        return success();
      }
      if (auto source = findRegStorage(op->getResult(0))) {
        op->setAttr("pyc.observe_lazy", BoolAttr::get(op->getContext(), true));
        if (failed(appendLazySlice(source->storage, name.getValue(), source->lsb,
                                   width, source->tapDepth)))
          return failure();
        ++lazy;
        return success();
      }
      auto host = findWireHost(op->getResult(0));
      if (!host) {
        ++kept;
        return success();
      }
      op->setAttr("pyc.observe_lazy", BoolAttr::get(op->getContext(), true));
      if (failed(appendLazyWire(host->op, name.getValue(), width,
                               host->resultIndex)))
        return failure();
      ++lazy;
      return success();
    };
    LogicalResult status = success();
    function.walk([&](Operation *op) {
      if (failed(status))
        return;
      status = mark(op);
    });
    if (failed(status))
      return failure();

    llvm::StringSet<> presentAfter;
    function.walk([&](Operation *op) {
      if (auto name = op->getAttrOfType<StringAttr>("pyc.name"))
        presentAfter.insert(name.getValue());
    });
    for (const auto &field : demand) {
      if (presentBefore.contains(field.getKey()) &&
          !presentAfter.contains(field.getKey()))
        return function.emitError(
                   "observation demand lost pyc.name after rewrite: ")
               << field.getKey();
    }

    setI64Attr(function, "pyc.stats.observe_named_names_stripped", lazy);
    setI64Attr(function, "pyc.stats.observe_named_names_lazy", lazy);
    setI64Attr(function, "pyc.stats.observe_named_names_kept", kept);
    return success();
  }
};

} // namespace

std::unique_ptr<Pass>
createApplyObservationDemandPass(std::string probePlanPath,
                                 std::string traceCodegenPlanPath) {
  return std::make_unique<ApplyObservationDemandPass>(
      std::move(probePlanPath), std::move(traceCodegenPlanPath));
}

static PassRegistration<ApplyObservationDemandPass> pass;

} // namespace pyc
