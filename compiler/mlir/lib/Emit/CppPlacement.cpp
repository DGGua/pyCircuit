#include "pyc/Emit/CppPlacement.h"

#include "mlir/IR/Attributes.h"
#include "mlir/IR/BuiltinAttributes.h"
#include "mlir/IR/BuiltinOps.h"
#include "mlir/IR/BuiltinTypes.h"
#include "mlir/IR/Operation.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/Support/raw_ostream.h"

using namespace mlir;

namespace pyc {

std::string combSortKey(pyc::CombOp comb) {
  if (auto nameAttr = comb->getAttrOfType<StringAttr>("pyc.name"))
    return nameAttr.getValue().str();
  if (comb.getNumResults() > 0) {
    if (auto nameAttr = comb.getResult(0).getDefiningOp()->getAttrOfType<StringAttr>("pyc.name"))
      return nameAttr.getValue().str();
  }
  std::string locKey;
  llvm::raw_string_ostream locOs(locKey);
  comb.getLoc().print(locOs);
  return locOs.str();
}

void sortCombsByStableKey(llvm::SmallVectorImpl<pyc::CombOp> &combs) {
  llvm::sort(combs, [](pyc::CombOp a, pyc::CombOp b) { return combSortKey(a) < combSortKey(b); });
}

namespace {

static Operation *definingOp(Value v) { return v.getDefiningOp(); }

static void setOnDefiningOp(Value v, CppStorageKind kind, StringRef owner, StringRef shard) {
  Operation *op = definingOp(v);
  if (!op)
    return;
  auto *ctx = op->getContext();
  op->setAttr(kCppStorageAttr, StringAttr::get(ctx, kind == CppStorageKind::Local ? "local" : "struct"));
  if (!owner.empty())
    op->setAttr(kCppOwnerAttr, StringAttr::get(ctx, owner));
  else
    op->removeAttr(kCppOwnerAttr);
  if (!shard.empty())
    op->setAttr(kCppShardAttr, StringAttr::get(ctx, shard));
  else
    op->removeAttr(kCppShardAttr);
}

static std::string cppTypeForWire(Type ty) {
  if (isa<pyc::ClockType>(ty) || isa<pyc::ResetType>(ty))
    return "pyc::cpp::Wire<1>";
  if (auto intTy = dyn_cast<IntegerType>(ty))
    return "pyc::cpp::Wire<" + std::to_string(intTy.getWidth()) + ">";
  return "pyc::cpp::Wire<1>";
}

static std::string methodForCombPart(unsigned combIdx, unsigned partIdx, bool hasParts) {
  if (hasParts)
    return "eval_comb_" + std::to_string(combIdx) + "_part_" + std::to_string(partIdx);
  return "eval_comb_" + std::to_string(combIdx);
}

static void assignCombOpMethods(pyc::CombOp comb, unsigned combIdx, unsigned combChunkNodes,
                                llvm::DenseMap<Operation *, std::string> &opToMethod,
                                llvm::DenseMap<pyc::CombOp, std::string> &combWrappers) {
  combWrappers[comb] = "eval_comb_" + std::to_string(combIdx);

  Block &b = comb.getBody().front();
  llvm::SmallVector<Operation *> combOps;
  for (Operation &op : b) {
    if (isa<pyc::YieldOp>(op))
      break;
    combOps.push_back(&op);
  }

  const bool chunk = combOps.size() > combChunkNodes;
  if (!chunk) {
    std::string m = methodForCombPart(combIdx, 0, false);
    for (Operation *op : combOps)
      opToMethod[op] = m;
    return;
  }

  for (unsigned begin = 0, partIdx = 0; begin < combOps.size(); begin += combChunkNodes, ++partIdx) {
    unsigned end = std::min<unsigned>(combOps.size(), begin + combChunkNodes);
    std::string m = methodForCombPart(combIdx, partIdx, true);
    for (unsigned i = begin; i < end; ++i)
      opToMethod[combOps[i]] = m;
  }
}

static llvm::SmallVector<pyc::CombOp> collectSortedTopLevelCombs(func::FuncOp f) {
  llvm::SmallVector<pyc::CombOp> combs;
  if (f.getBody().empty())
    return combs;
  Block &top = f.getBody().front();
  for (Operation &op : top)
    if (auto comb = dyn_cast<pyc::CombOp>(op))
      combs.push_back(comb);
  sortCombsByStableKey(combs);
  return combs;
}

static bool pinToStruct(Value v) {
  if (!v.getDefiningOp())
    return true;

  Operation *def = v.getDefiningOp();
  if (isa<pyc::CombOp>(def))
    return true;
  if (isa<pyc::RegOp, pyc::InstanceOp, pyc::FifoOp, pyc::ByteMemOp, pyc::SyncMemOp, pyc::SyncMemDPOp,
            pyc::AsyncFifoOp, pyc::CdcSyncOp>(def))
    return true;

  if (def->getParentOfType<pyc::CombOp>()) {
    for (OpOperand &use : v.getUses()) {
      Operation *user = use.getOwner();
      if (!user->getParentOfType<pyc::CombOp>())
        return true;
    }
  }
  return false;
}

static StringRef ownerOfValue(Value v) {
  StringRef owner = getValueCppOwner(v);
  return owner.empty() ? StringRef("core") : owner;
}

static StringRef methodForUserOp(Operation *user,
                                 const llvm::DenseMap<Operation *, std::string> &opToMethod,
                                 const llvm::DenseMap<pyc::CombOp, std::string> &combWrappers) {
  if (auto it = opToMethod.find(user); it != opToMethod.end())
    return it->second;
  if (isa<pyc::YieldOp>(user)) {
    if (auto comb = user->getParentOfType<pyc::CombOp>()) {
      if (auto it = combWrappers.find(comb); it != combWrappers.end())
        return it->second;
    }
  }
  return "core";
}

static void buildCombMethodMaps(func::FuncOp f, unsigned combChunkNodes,
                                llvm::DenseMap<Operation *, std::string> &opToMethod,
                                llvm::DenseMap<pyc::CombOp, std::string> &combWrappers) {
  llvm::SmallVector<pyc::CombOp> combs = collectSortedTopLevelCombs(f);
  for (auto [i, comb] : llvm::enumerate(combs))
    assignCombOpMethods(comb, static_cast<unsigned>(i), combChunkNodes, opToMethod, combWrappers);
}

} // namespace

CppStorageKind getValueCppStorage(Value v) {
  Operation *op = definingOp(v);
  if (!op)
    return CppStorageKind::Struct;
  if (auto a = op->getAttrOfType<StringAttr>(kCppStorageAttr)) {
    if (a.getValue() == "local")
      return CppStorageKind::Local;
  }
  return CppStorageKind::Struct;
}

StringRef getValueCppOwner(Value v) {
  Operation *op = definingOp(v);
  if (!op)
    return {};
  if (auto a = op->getAttrOfType<StringAttr>(kCppOwnerAttr))
    return a.getValue();
  return {};
}

void setValueCppPlacement(Value v, CppStorageKind kind, StringRef owner, StringRef shard) {
  setOnDefiningOp(v, kind, owner, shard);
}

bool CppEmitterPlacementState::emitLocalDeclIfNeeded(Value v, Type ty, StringRef name,
                                                   llvm::raw_ostream &os, unsigned indentSpaces) {
  if (getValueCppStorage(v) != CppStorageKind::Local)
    return false;
  StringRef owner = getValueCppOwner(v);
  if (!owner.empty() && owner != currentMethod)
    return false;
  if (!declaredLocals.insert(v).second)
    return false;
  for (unsigned i = 0; i < indentSpaces; ++i)
    os << ' ';
  os << cppTypeForWire(ty) << " " << name << "{};\n";
  return true;
}

void scheduleCppCombMethods(func::FuncOp f, unsigned combChunkNodes) {
  if (combChunkNodes == 0)
    return;

  llvm::DenseMap<Operation *, std::string> opToMethod;
  llvm::DenseMap<pyc::CombOp, std::string> combWrappers;
  buildCombMethodMaps(f, combChunkNodes, opToMethod, combWrappers);

  f.walk([&](Operation *op) {
    if (op->getParentOfType<pyc::CombOp>() == nullptr)
      return;
    if (isa<pyc::YieldOp>(op))
      return;
    auto it = opToMethod.find(op);
    if (it == opToMethod.end())
      return;
    for (Value r : op->getResults())
      setValueCppPlacement(r, CppStorageKind::Struct, it->second, "comb");
  });
}

CppPlacementSummary localizeCppCombMembers(func::FuncOp f, unsigned combChunkNodes) {
  CppPlacementSummary summary;

  llvm::DenseMap<Operation *, std::string> opToMethod;
  llvm::DenseMap<pyc::CombOp, std::string> combWrappers;
  if (combChunkNodes > 0)
    buildCombMethodMaps(f, combChunkNodes, opToMethod, combWrappers);

  llvm::SmallVector<Value> candidates;
  f.walk([&](Operation *op) {
    if (op->getParentOfType<pyc::CombOp>() == nullptr)
      return;
    if (isa<pyc::YieldOp>(op))
      return;
    for (Value r : op->getResults())
      candidates.push_back(r);
  });

  for (Value v : candidates) {
    if (pinToStruct(v)) {
      setValueCppPlacement(v, CppStorageKind::Struct, {});
      summary.structMembers++;
      if (!v.getDefiningOp())
        summary.probePinnedStruct++;
      continue;
    }

    Operation *def = v.getDefiningOp();
    if (!def) {
      setValueCppPlacement(v, CppStorageKind::Struct, {});
      summary.structMembers++;
      continue;
    }

    const StringRef ownerM = ownerOfValue(v);
    bool crossMethod = false;
    for (OpOperand &use : v.getUses()) {
      Operation *user = use.getOwner();
      if (methodForUserOp(user, opToMethod, combWrappers) != ownerM) {
        crossMethod = true;
        break;
      }
    }
    if (crossMethod) {
      setValueCppPlacement(v, CppStorageKind::Struct, ownerM, "comb");
      summary.structMembers++;
      summary.promotedCrossMethod++;
      continue;
    }

    setValueCppPlacement(v, CppStorageKind::Local, ownerM, "comb");
    summary.localInMethod++;
  }

  f.walk([&](Operation *op) {
    if (op->getParentOfType<pyc::CombOp>() != nullptr)
      return;
    for (Value r : op->getResults()) {
      if (getValueCppStorage(r) == CppStorageKind::Local)
        continue;
      setValueCppPlacement(r, CppStorageKind::Struct, {});
      summary.structMembers++;
    }
  });

  return summary;
}

CppPlacementSummary runCppMemberPlacement(func::FuncOp f, unsigned combChunkNodes) {
  scheduleCppCombMethods(f, combChunkNodes);
  return localizeCppCombMembers(f, combChunkNodes);
}

void setModuleCombChunkNodes(ModuleOp module, unsigned combChunkNodes) {
  auto *ctx = module.getContext();
  module->setAttr(kCppCombChunkNodesAttr,
                  IntegerAttr::get(IntegerType::get(ctx, 64), combChunkNodes));
}

std::optional<unsigned> getModuleCombChunkNodes(ModuleOp module) {
  auto attr = module->getAttrOfType<IntegerAttr>(kCppCombChunkNodesAttr);
  if (!attr)
    return std::nullopt;
  return static_cast<unsigned>(attr.getValue().getZExtValue());
}

void setFuncPlacementSummary(func::FuncOp f, const CppPlacementSummary &summary) {
  auto *ctx = f.getContext();
  llvm::SmallVector<NamedAttribute, 4> fields;
  fields.emplace_back(StringAttr::get(ctx, "struct_members"),
                      IntegerAttr::get(IntegerType::get(ctx, 64), summary.structMembers));
  fields.emplace_back(StringAttr::get(ctx, "local_in_method"),
                      IntegerAttr::get(IntegerType::get(ctx, 64), summary.localInMethod));
  fields.emplace_back(StringAttr::get(ctx, "promoted_cross_method"),
                      IntegerAttr::get(IntegerType::get(ctx, 64), summary.promotedCrossMethod));
  fields.emplace_back(StringAttr::get(ctx, "probe_pinned_struct"),
                      IntegerAttr::get(IntegerType::get(ctx, 64), summary.probePinnedStruct));
  f->setAttr(kCppPlacementSummaryAttr, DictionaryAttr::get(ctx, fields));
}

static std::optional<uint64_t> readSummaryField(func::FuncOp f, StringRef key) {
  auto dict = f->getAttrOfType<DictionaryAttr>(kCppPlacementSummaryAttr);
  if (!dict)
    return std::nullopt;
  auto attr = dict.get(key);
  if (!attr)
    return std::nullopt;
  if (auto intAttr = dyn_cast<IntegerAttr>(attr))
    return intAttr.getValue().getZExtValue();
  return std::nullopt;
}

std::optional<CppPlacementSummary> getFuncPlacementSummary(func::FuncOp f) {
  auto structMembers = readSummaryField(f, "struct_members");
  if (!structMembers)
    return std::nullopt;
  CppPlacementSummary summary;
  summary.structMembers = static_cast<unsigned>(*structMembers);
  if (auto v = readSummaryField(f, "local_in_method"))
    summary.localInMethod = static_cast<unsigned>(*v);
  if (auto v = readSummaryField(f, "promoted_cross_method"))
    summary.promotedCrossMethod = static_cast<unsigned>(*v);
  if (auto v = readSummaryField(f, "probe_pinned_struct"))
    summary.probePinnedStruct = static_cast<unsigned>(*v);
  return summary;
}

CppPlacementSummary accumulateModulePlacementSummary(ModuleOp module) {
  CppPlacementSummary totals;
  for (auto f : module.getOps<func::FuncOp>()) {
    if (f.isDeclaration())
      continue;
    if (auto summary = getFuncPlacementSummary(f)) {
      totals.structMembers += summary->structMembers;
      totals.localInMethod += summary->localInMethod;
      totals.promotedCrossMethod += summary->promotedCrossMethod;
      totals.probePinnedStruct += summary->probePinnedStruct;
    }
  }
  return totals;
}

void CppEmitterPlacementState::emitValueAssign(Value result, Type ty, StringRef name, StringRef expr,
                                               llvm::raw_ostream &os, unsigned indentSpaces) {
  for (unsigned i = 0; i < indentSpaces; ++i)
    os << ' ';
  if (getValueCppStorage(result) != CppStorageKind::Local) {
    os << name << " = " << expr << ";\n";
    return;
  }
  StringRef owner = getValueCppOwner(result);
  if (!owner.empty() && owner != currentMethod) {
    // Placement owner indices can disagree with emitter comb order; still declare
    // the SSA temp in the method that defines it.
    if (declaredLocals.insert(result).second)
      os << cppTypeForWire(ty) << " " << name << " = " << expr << ";\n";
    else
      os << name << " = " << expr << ";\n";
    return;
  }
  // SSA: one defining assign per Value; further uses only read `name`. If an operand
  // prep emitted `{}` via emitLocalDeclIfNeeded, the defining op takes this branch.
  if (declaredLocals.insert(result).second)
    os << cppTypeForWire(ty) << " " << name << " = " << expr << ";\n";
  else
    os << name << " = " << expr << ";\n";
}

} // namespace pyc
