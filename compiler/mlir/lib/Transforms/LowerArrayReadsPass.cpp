//===- LowerArrayReadsPass.cpp - eq+mux read chains -> pyc.array_read -----===//
//
// Recognizes the classic "read-port" pattern emitted for register files and
// other state arrays:
//
//   %c128   = pyc.constant 128 : i8
//   %eq0    = pyc.eq %addr, %c128 : i8, i8 -> i1
//   %m0     = pyc.mux %eq0, %slot0, %fallback : i1, i32, i32 -> i32
//   %eq1    = pyc.eq %addr, %c129 : i8, i8 -> i1
//   %m1     = pyc.mux %eq1, %slot1, %m0 : i1, i32, i32 -> i32
//   ...
//
// and collapses each linear chain into a single `pyc.array_read` op so the
// C++ emitter can emit a direct array-indexed read (`arr[addr - base]`)
// instead of hundreds of nested mux calls. The Verilog emitter re-expands the
// op into the equivalent ternary chain, so both backends stay semantically
// identical (G2 equivalence is preserved by construction).
//
// Semantics of the collapsed op (also enforced by its verifier):
//   result = (addr >= base && addr < base + count) ? slots[addr - base] : fallback
//
//===----------------------------------------------------------------------===//

#include "mlir/IR/Builders.h"
#include "mlir/IR/IRMapping.h"
#include "mlir/IR/Matchers.h"
#include "mlir/Pass/Pass.h"
#include "mlir/Support/LLVM.h"
#include "mlir/Dialect/Func/IR/FuncOps.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/SmallVector.h"
#include "pyc/Dialect/PYC/PYCDialect.h"
#include "pyc/Dialect/PYC/PYCOps.h"
#include "pyc/Transforms/Passes.h"

using namespace mlir;

namespace pyc {
namespace {

// Resolve `v` to an integer constant, following pyc.alias chains.
static std::optional<int64_t> constantInt(Value v) {
  while (true) {
    if (auto c = v.getDefiningOp<pyc::ConstantOp>()) {
      if (auto i = dyn_cast<IntegerAttr>(c.getValueAttr()))
        return i.getValue().getZExtValue();
      return std::nullopt;
    }
    if (auto a = v.getDefiningOp<pyc::AliasOp>()) {
      v = a.getIn();
      continue;
    }
    return std::nullopt;
  }
}

// One collapsed read-port chain.
struct Chain {
  Value addr;
  Value fallback;
  SmallVector<Value> slots; // slots[i] selected when addr == base + i
  int64_t base = 0;
  int64_t count = 0;
  SmallVector<pyc::MuxOp> muxes;  // every mux of the chain (for erasure)
  SmallVector<pyc::EqOp> eqs;     // every eq of the chain (for erasure)
};

class LowerArrayReadsPass
    : public PassWrapper<LowerArrayReadsPass, OperationPass<func::FuncOp>> {
public:
  MLIR_DEFINE_EXPLICIT_INTERNAL_INLINE_TYPE_ID(LowerArrayReadsPass)

  // Only collapse chains with at least this many slots; smaller chains are
  // cheaper as mux trees than as an extra array copy.
  static constexpr int64_t kMinCount = 8;

  StringRef getArgument() const override { return "pyc-lower-array-reads"; }
  StringRef getDescription() const override {
    return "Collapse eq+mux read-port chains into pyc.array_read (array-indexed C++ reads)";
  }

  void runOnOperation() override {
    func::FuncOp f = getOperation();
    SmallVector<Chain> chains = findChains(f);
    if (chains.empty())
      return;
    rewriteChains(f, chains);
  }

private:
  // addr -> [(constant, eq result)]
  using EqInfo = SmallVector<std::pair<int64_t, Value>>;
  using MuxSet = DenseSet<Operation *>;

  SmallVector<Chain> findChains(func::FuncOp f) {
    SmallVector<Chain> result;

    // 1. Collect eq ops with one constant operand: eq(addr, const).
    DenseMap<Value, EqInfo> eqsByAddr;          // addr -> [(const, eqResult)]
    DenseMap<Value, std::pair<Value, int64_t>> eqInfo; // eqResult -> (addr, const)
    f.walk([&](pyc::EqOp eq) {
      auto lc = constantInt(eq.getLhs());
      auto rc = constantInt(eq.getRhs());
      Value addr;
      int64_t c;
      if (lc && !rc) {
        addr = eq.getRhs();
        c = *lc;
      } else if (rc && !lc) {
        addr = eq.getLhs();
        c = *rc;
      } else {
        return; // both/neither constant: not a read-port select
      }
      eqsByAddr[addr].push_back({c, eq.getResult()});
      eqInfo[eq.getResult()] = {addr, c};
    });
    if (eqsByAddr.empty())
      return result;

    // 2. muxes selected by those eqs.
    DenseMap<Value, SmallVector<pyc::MuxOp>> muxesBySel;
    SmallVector<pyc::MuxOp> allMuxes;
    f.walk([&](pyc::MuxOp m) {
      allMuxes.push_back(m);
      if (eqInfo.contains(m.getSel()))
        muxesBySel[m.getSel()].push_back(m);
    });

    // 3. For each addr, group its chain muxes and build linear chains.
    for (auto &[addr, eqList] : eqsByAddr) {
      // muxSet: all muxes selected by this addr's eqs.
      MuxSet muxSet;
      SmallVector<pyc::MuxOp> groupMuxes;
      for (auto &[c, sel] : eqList) {
        (void)c;
        for (pyc::MuxOp m : muxesBySel[sel]) {
          if (muxSet.insert(m).second)
            groupMuxes.push_back(m);
        }
      }
      if (groupMuxes.size() < static_cast<size_t>(kMinCount))
        continue;

      // Chain roots: mux results used by ops outside the group. A group may
      // contain several independent chains (e.g. a low and a high word of the
      // same state array); each root starts its own chain.
      SmallVector<pyc::MuxOp> roots;
      for (pyc::MuxOp m : groupMuxes)
        if (llvm::any_of(m.getResult().getUsers(), [&](Operation *u) {
              return !muxSet.contains(u);
            }))
          roots.push_back(m);

      DenseSet<Operation *> consumedMuxes;
      for (pyc::MuxOp root : roots) {
        // Walk root -> tail following the non-slot link. Slot operands are the
        // branch that is not another chain mux; the tail's non-link operand is
        // the fallback. Constants must be contiguous and strictly decreasing
        // along the walk (root = highest constant).
        SmallVector<std::pair<int64_t, Value>> rev; // (const, slot) root..tail
        SmallVector<pyc::MuxOp> chainMuxes;
        Value fallback;
        pyc::MuxOp cur = root;
        bool valid = true;
        while (cur) {
          if (consumedMuxes.contains(cur)) {
            valid = false; // already part of another chain
            break;
          }
          auto it = eqInfo.find(cur.getSel());
          if (it == eqInfo.end()) {
            valid = false;
            break;
          }
          int64_t c = it->second.second;
          Value a = cur.getA();
          Value b = cur.getB();
          bool aLink = a.getDefiningOp() && muxSet.contains(a.getDefiningOp());
          bool bLink = b.getDefiningOp() && muxSet.contains(b.getDefiningOp());
          if (aLink && bLink) {
            valid = false; // fork: both operands are chain links
            break;
          }
          Value slot = aLink ? b : a;
          pyc::MuxOp next = (aLink ? dyn_cast<pyc::MuxOp>(a.getDefiningOp())
                                   : dyn_cast<pyc::MuxOp>(b.getDefiningOp()));
          if (!next && (aLink || bLink)) {
            valid = false;
            break;
          }
          chainMuxes.push_back(cur);
          rev.push_back({c, slot});
          if (!aLink && !bLink) {
            fallback = b; // tail: mux(sel, slot, fallback)
            break;
          }
          cur = next;
        }
        if (!valid || rev.size() < static_cast<size_t>(kMinCount))
          continue;

        // Constants must be contiguous: rev is root..tail with strictly
        // decreasing constants; reversed gives ascending base..base+count-1.
        SmallVector<std::pair<int64_t, Value>> asc(rev.rbegin(), rev.rend());
        bool contiguous = true;
        for (size_t i = 1; i < asc.size(); ++i)
          if (asc[i].first != asc[i - 1].first + 1) {
            contiguous = false;
            break;
          }
        if (!contiguous)
          continue;

        // All slots and the fallback must share the result type (integer).
        Type resTy = root.getResult().getType();
        if (!isa<IntegerType>(resTy))
          continue;
        bool typesOk = fallback && fallback.getType() == resTy;
        for (auto &[c, slot] : asc)
          typesOk = typesOk && slot.getType() == resTy;
        if (!typesOk)
          continue;

        Chain chain;
        chain.addr = addr;
        chain.fallback = fallback;
        chain.base = asc.front().first;
        chain.count = static_cast<int64_t>(asc.size());
        for (auto &[c, slot] : asc)
          chain.slots.push_back(slot);
        chain.muxes = std::move(chainMuxes);
        // Collect every eq used by this chain (for erasure).
        for (pyc::MuxOp m : chain.muxes)
          if (auto eqOp = m.getSel().getDefiningOp<pyc::EqOp>())
            if (!llvm::is_contained(chain.eqs, eqOp))
              chain.eqs.push_back(eqOp);
        for (pyc::MuxOp m : chain.muxes)
          consumedMuxes.insert(m);
        result.push_back(std::move(chain));
      }
    }
    return result;
  }

  void rewriteChains(func::FuncOp f, SmallVector<Chain> &chains) {
    OpBuilder builder(f.getContext());
    DenseSet<Operation *> erasedEqs;
    for (Chain &ch : chains) {
      builder.setInsertionPoint(ch.muxes.front());
      SmallVector<Value> slotVals(ch.slots.begin(), ch.slots.end());
      auto ar = builder.create<pyc::ArrayReadOp>(
          ch.muxes.front()->getLoc(), ch.muxes.front()->getResult(0).getType(),
          ch.addr, ch.fallback, slotVals, builder.getI64IntegerAttr(ch.base),
          builder.getI64IntegerAttr(ch.count));
      ch.muxes.front()->getResult(0).replaceAllUsesWith(ar.getResult());
      for (pyc::MuxOp m : ch.muxes)
        if (m.use_empty())
          m.erase();
      for (pyc::EqOp e : ch.eqs)
        if (e.use_empty() && erasedEqs.insert(e).second)
          e.erase();
    }
  }
};

} // namespace

std::unique_ptr<mlir::Pass> createLowerArrayReadsPass() {
  return std::make_unique<LowerArrayReadsPass>();
}

static PassRegistration<LowerArrayReadsPass> pass;

} // namespace pyc
