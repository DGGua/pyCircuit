#include "pyc/Transforms/Passes.h"
#include "pyc/Transforms/StateOptimization.h"

#include "mlir/Dialect/Func/IR/FuncOps.h"
#include "mlir/IR/BuiltinOps.h"
#include "mlir/Pass/Pass.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/ADT/StringSet.h"
#include "llvm/Support/JSON.h"
#include "llvm/Support/MemoryBuffer.h"

#include <string>

using namespace mlir;

namespace pyc {
namespace {

static void setI64Attr(Operation *op, StringRef name, int64_t value) {
  op->setAttr(name, IntegerAttr::get(IntegerType::get(op->getContext(), 64),
                                     value));
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

// After merge/retime/pack have remapped every author identity, drop pyc.name
// values that nobody asked to read. Decision 0091/0096: unselected probes
// must not pay C++ struct/addReg/fuse-comb cost. IR attrs debug_keep /
// observable / probe* / trace* still count as demand.
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
    return "Strip undemanded pyc.name after state opt so C++ probes stay "
           "selective";
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

    int64_t stripped = 0;
    int64_t kept = 0;
    function.walk([&](Operation *op) {
      auto name = op->getAttrOfType<StringAttr>("pyc.name");
      if (!name)
        return;
      const bool demanded = shouldKeepStateOptimization(op) ||
                            demand.contains(name.getValue());
      if (demanded) {
        ++kept;
        return;
      }
      op->removeAttr("pyc.name");
      ++stripped;
    });

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

    setI64Attr(function, "pyc.stats.observe_named_names_stripped", stripped);
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
