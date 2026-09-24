#include "pyc/Transforms/Passes.h"
#include "pyc/Transforms/CombDepGraph.h"

#include "pyc/Dialect/PYC/PYCOps.h"

#include "mlir/Dialect/Arith/IR/Arith.h"
#include "mlir/IR/BuiltinAttributes.h"
#include "mlir/Dialect/Func/IR/FuncOps.h"
#include "mlir/IR/BuiltinOps.h"
#include "mlir/IR/SymbolTable.h"
#include "mlir/Pass/Pass.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/SmallVector.h"

#include <algorithm>
#include <limits>

using namespace mlir;

namespace pyc {
namespace {

static bool isSequentialOp(Operation *op) {
  return isa<pyc::RegOp,
             pyc::FifoOp,
             pyc::ByteMemOp,
             pyc::SyncMemOp,
             pyc::SyncMemDPOp,
             pyc::AsyncFifoOp,
             pyc::CdcSyncOp>(op);
}

static int64_t ceilLog2(int64_t n) {
  if (n <= 1)
    return 0;
  int64_t depth = 0;
  int64_t v = 1;
  while (v < n) {
    v <<= 1;
    ++depth;
  }
  return depth;
}

template <typename ReduceOp>
static int64_t vectorReduceCost(ReduceOp op) {
  auto vecTy = dyn_cast<VectorType>(op.getVec().getType());
  if (!vecTy)
    return 1;
  int64_t lanes = 1;
  if (auto dim = op.getDim()) {
    if (*dim < 0 || *dim >= vecTy.getRank())
      return 1;
    lanes = vecTy.getDimSize(*dim);
  } else {
    for (int64_t extent : vecTy.getShape())
      lanes *= extent;
  }
  if (auto mode = op->template getAttrOfType<StringAttr>("mode")) {
    if (mode.getValue() == "tree")
      return std::max<int64_t>(1, ceilLog2(lanes));
  }
  return std::max<int64_t>(1, lanes - 1);
}

static int64_t opCost(Operation *op) {
  if (!op)
    return 0;
  if (isSequentialOp(op))
    return 0;
  if (isa<pyc::WireOp, pyc::AliasOp, pyc::ResetActiveOp, pyc::ConstantOp, pyc::CombOp, pyc::YieldOp,
          arith::ConstantOp>(op))
    return 0;
  if (isa<pyc::VGetOp, pyc::VCreateOp, pyc::VBroadcastOp>(op))
    return 0;
  if (auto vr = dyn_cast<pyc::VOrReduceOp>(op))
    return vectorReduceCost(vr);
  if (auto vr = dyn_cast<pyc::VAndReduceOp>(op))
    return vectorReduceCost(vr);
  if (auto vr = dyn_cast<pyc::VAddReduceOp>(op))
    return vectorReduceCost(vr);
  return 1;
}

class CheckLogicDepthPass : public PassWrapper<CheckLogicDepthPass, OperationPass<ModuleOp>> {
public:
  MLIR_DEFINE_EXPLICIT_INTERNAL_INLINE_TYPE_ID(CheckLogicDepthPass)

  explicit CheckLogicDepthPass(unsigned depth = 32) : maxDepthLimit(depth) {}

  StringRef getArgument() const override { return "pyc-check-logic-depth"; }
  StringRef getDescription() const override {
    return "Check strict combinational depth and compute WNS/TNS against LOGIC_DEPTH";
  }

  void runOnOperation() override {
    ModuleOp module = getOperation();
    CombDepGraphCache combCache(module);

    bool failedAny = false;

    for (func::FuncOp f : module.getOps<func::FuncOp>()) {
      const int64_t limit = static_cast<int64_t>(maxDepthLimit);

      llvm::DenseMap<Value, llvm::SmallVector<Value>> wireDrivers;
      f.walk([&](pyc::AssignOp a) {
        Value dst = a.getDst();
        if (!dst || !dst.getDefiningOp<pyc::WireOp>())
          return;
        wireDrivers[dst].push_back(a.getSrc());
      });

      llvm::DenseMap<Value, int64_t> memo;
      llvm::DenseSet<Value> visiting;
      bool failedThisFunc = false;

      auto dependencies = [&](Value v, llvm::SmallVectorImpl<Value> &out) {
        if (auto barg = dyn_cast<BlockArgument>(v)) {
          Operation *parent = barg.getOwner() ? barg.getOwner()->getParentOp() : nullptr;
          if (auto comb = dyn_cast_or_null<pyc::CombOp>(parent)) {
            unsigned idx = barg.getArgNumber();
            if (idx < comb.getInputs().size())
              out.push_back(comb.getInputs()[idx]);
          }
          return;
        }
        Operation *def = v.getDefiningOp();
        if (!def || isSequentialOp(def) || isa<arith::ConstantOp, pyc::ConstantOp>(def))
          return;
        if (isa<pyc::WireOp>(def)) {
          if (auto it = wireDrivers.find(v); it != wireDrivers.end())
            out.append(it->second.begin(), it->second.end());
        } else if (auto alias = dyn_cast<pyc::AliasOp>(def)) {
          out.push_back(alias.getIn());
        } else if (auto comb = dyn_cast<pyc::CombOp>(def)) {
          auto result = dyn_cast<OpResult>(v);
          unsigned index = result ? result.getResultNumber() : 0u;
          auto yield = comb.getBody().empty()
                           ? pyc::YieldOp()
                           : dyn_cast_or_null<pyc::YieldOp>(comb.getBody().front().getTerminator());
          if (yield && index < yield.getValues().size())
            out.push_back(yield.getValues()[index]);
        } else if (auto inst = dyn_cast<pyc::InstanceOp>(def)) {
          auto result = dyn_cast<OpResult>(v);
          unsigned index = result ? result.getResultNumber() : 0u;
          auto calleeAttr = inst->getAttrOfType<FlatSymbolRefAttr>("callee");
          auto callee = calleeAttr
                            ? dyn_cast_or_null<func::FuncOp>(SymbolTable::lookupSymbolIn(module, calleeAttr.getValue()))
                            : func::FuncOp();
          if (callee) {
            if (const FuncCombSummary *sum = combCache.getFuncSummary(callee)) {
              if (index < sum->results.size()) {
                const auto &argDepth = sum->results[index].argDepth;
                auto inputs = inst.getInputs();
                unsigned n = std::min<unsigned>(inputs.size(), argDepth.size());
                for (unsigned i = 0; i < n; ++i)
                  if (argDepth[i] >= 0)
                    out.push_back(inputs[i]);
              }
            }
          }
        } else {
          out.append(def->operand_begin(), def->operand_end());
        }
      };

      struct DepthFrame {
        Value value;
        llvm::SmallVector<Value> inputs;
        size_t next = 0;
      };
      auto depthOf = [&](Value root) -> int64_t {
        if (!root)
          return 0;
        if (auto it = memo.find(root); it != memo.end())
          return it->second;
        llvm::SmallVector<DepthFrame> stack;
        visiting.insert(root);
        stack.push_back({root, {}, 0});
        dependencies(root, stack.back().inputs);
        while (!stack.empty()) {
          DepthFrame &frame = stack.back();
          if (frame.next < frame.inputs.size()) {
            Value child = frame.inputs[frame.next++];
            if (!child || memo.count(child) || visiting.count(child))
              continue;
            visiting.insert(child);
            stack.push_back({child, {}, 0});
            dependencies(child, stack.back().inputs);
            continue;
          }

          auto getDepth = [&](Value child) -> int64_t {
            if (!child)
              return 0;
            if (auto it = memo.find(child); it != memo.end())
              return it->second;
            return limit + 1; // A combinational cycle reaches an active ancestor.
          };
          Value v = frame.value;
          int64_t d = 0;
          if (auto barg = dyn_cast<BlockArgument>(v)) {
            Operation *parent = barg.getOwner() ? barg.getOwner()->getParentOp() : nullptr;
            if (auto comb = dyn_cast_or_null<pyc::CombOp>(parent)) {
              unsigned idx = barg.getArgNumber();
              if (idx < comb.getInputs().size())
                d = getDepth(comb.getInputs()[idx]);
            }
          } else if (Operation *def = v.getDefiningOp()) {
            if (isSequentialOp(def) || isa<arith::ConstantOp, pyc::ConstantOp>(def)) {
              d = 0;
            } else if (isa<pyc::WireOp>(def)) {
              if (auto it = wireDrivers.find(v); it != wireDrivers.end())
                for (Value src : it->second)
                  d = std::max(d, getDepth(src));
            } else if (auto alias = dyn_cast<pyc::AliasOp>(def)) {
              d = getDepth(alias.getIn());
            } else if (auto comb = dyn_cast<pyc::CombOp>(def)) {
              auto result = dyn_cast<OpResult>(v);
              unsigned index = result ? result.getResultNumber() : 0u;
              auto yield = comb.getBody().empty()
                               ? pyc::YieldOp()
                               : dyn_cast_or_null<pyc::YieldOp>(comb.getBody().front().getTerminator());
              if (yield && index < yield.getValues().size())
                d = getDepth(yield.getValues()[index]);
            } else if (auto inst = dyn_cast<pyc::InstanceOp>(def)) {
              auto result = dyn_cast<OpResult>(v);
              unsigned index = result ? result.getResultNumber() : 0u;
              auto calleeAttr = inst->getAttrOfType<FlatSymbolRefAttr>("callee");
              if (!calleeAttr) {
                inst.emitError("pyc.instance missing required `callee` attr");
                failedThisFunc = true;
                d = limit + 1;
              } else {
                auto callee = dyn_cast_or_null<func::FuncOp>(SymbolTable::lookupSymbolIn(module, calleeAttr.getValue()));
                if (!callee) {
                  inst.emitError("pyc.instance callee is not a func.func symbol: ") << calleeAttr.getValue();
                  failedThisFunc = true;
                  d = limit + 1;
                } else if (const FuncCombSummary *sum = combCache.getFuncSummary(callee)) {
                  if (index < sum->results.size()) {
                    const CombResultSummary &rs = sum->results[index];
                    d = std::max<int64_t>(0, rs.baseDepth);
                    auto inputs = inst.getInputs();
                    unsigned n = std::min<unsigned>(inputs.size(), rs.argDepth.size());
                    for (unsigned i = 0; i < n; ++i)
                      if (rs.argDepth[i] >= 0)
                        d = std::max(d, getDepth(inputs[i]) + rs.argDepth[i]);
                  }
                } else if (!callee.isDeclaration()) {
                  failedThisFunc = true;
                  d = limit + 1;
                }
              }
            } else {
              for (Value in : def->getOperands())
                d = std::max(d, getDepth(in));
              d += opCost(def);
            }
          }
          memo.try_emplace(v, d);
          visiting.erase(v);
          stack.pop_back();
        }
        return memo.lookup(root);
      };

      int64_t maxDepth = 0;
      int64_t wns = std::numeric_limits<int64_t>::max();
      int64_t tns = 0;

      auto observeEndpoint = [&](Operation *op, Value v) {
        int64_t d = depthOf(v);
        maxDepth = std::max(maxDepth, d);
        int64_t slack = limit - d;
        wns = std::min(wns, slack);
        if (slack < 0)
          tns += slack;
        if (d > limit) {
          op->emitError("logic depth exceeds limit: depth=") << d << " limit=" << limit;
          failedThisFunc = true;
        }
      };

      f.walk([&](Operation *op) {
        if (auto ret = dyn_cast<func::ReturnOp>(op)) {
          for (Value v : ret.getOperands())
            observeEndpoint(op, v);
          return;
        }
        if (auto a = dyn_cast<pyc::AssertOp>(op)) {
          observeEndpoint(op, a.getCond());
          return;
        }
        if (isSequentialOp(op)) {
          for (Value v : op->getOperands())
            observeEndpoint(op, v);
        }
      });

      if (wns == std::numeric_limits<int64_t>::max())
        wns = limit;

      auto i64Ty = IntegerType::get(f.getContext(), 64);
      f->setAttr("pyc.logic_depth.max", IntegerAttr::get(i64Ty, maxDepth));
      f->setAttr("pyc.logic_depth.wns", IntegerAttr::get(i64Ty, wns));
      f->setAttr("pyc.logic_depth.tns", IntegerAttr::get(i64Ty, tns));

      if (failedThisFunc)
        failedAny = true;
    }

    if (failedAny)
      signalPassFailure();
  }

private:
  unsigned maxDepthLimit = 32;
};

} // namespace

std::unique_ptr<::mlir::Pass> createCheckLogicDepthPass(unsigned logicDepth) {
  return std::make_unique<CheckLogicDepthPass>(logicDepth);
}

static PassRegistration<CheckLogicDepthPass> pass;

} // namespace pyc
