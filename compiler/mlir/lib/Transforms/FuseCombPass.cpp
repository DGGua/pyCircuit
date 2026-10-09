#include "pyc/Transforms/Passes.h"
#include "pyc/Transforms/StateOptimization.h"

#include "pyc/Dialect/PYC/PYCOps.h"

#include "mlir/Dialect/Arith/IR/Arith.h"
#include "mlir/Dialect/Func/IR/FuncOps.h"
#include "mlir/IR/Builders.h"
#include "mlir/IR/IRMapping.h"
#include "mlir/Pass/Pass.h"
#include "mlir/IR/BuiltinAttributes.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/SmallSet.h"
#include <functional>
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/STLExtras.h"

using namespace mlir;

namespace pyc {
namespace {

static bool isFusableCombOp(Operation *op) {
  if (shouldKeepStateOptimization(op) || hasStableStateName(op))
    return false;
  return isa<pyc::ConstantOp,
             pyc::AddOp,
             pyc::SubOp,
             pyc::MulOp,
             pyc::UdivOp,
             pyc::UremOp,
             pyc::SdivOp,
             pyc::SremOp,
             pyc::MuxOp,
             pyc::AndOp,
             pyc::OrOp,
             pyc::XorOp,
             pyc::NotOp,
             pyc::ConcatOp,
             pyc::AliasOp,
             pyc::ResetActiveOp,
             pyc::EqOp,
             pyc::UltOp,
             pyc::SltOp,
             pyc::TruncOp,
             pyc::ZextOp,
             pyc::SextOp,
             pyc::ExtractOp,
             pyc::ShliOp,
             pyc::LshriOp,
             pyc::AshriOp,
             pyc::ShlOp,
             pyc::LshrOp,
             pyc::AshrOp,
             pyc::VGetOp,
             pyc::VCreateOp,
             pyc::VBroadcastOp,
             pyc::VOrReduceOp,
             pyc::VAndReduceOp,
             pyc::VAddReduceOp,
             arith::SelectOp>(op);
}

struct FuseCombPass : public PassWrapper<FuseCombPass, OperationPass<func::FuncOp>> {
  MLIR_DEFINE_EXPLICIT_INTERNAL_INLINE_TYPE_ID(FuseCombPass)

  FuseCombPass() = default;
  explicit FuseCombPass(uint64_t smallPartitionThreshold)
      : smallPartitionThreshold(smallPartitionThreshold) {}

  StringRef getArgument() const override { return "pyc-fuse-comb"; }
  StringRef getDescription() const override {
    return "Fuse pyc combinational ops into pyc.comb regions (textual runs, "
           "or ESSENT acyclic partitions when a partition threshold is set)";
  }

  void runOnOperation() override {
    func::FuncOp f = getOperation();
    if (auto structural = f->getAttrOfType<StringAttr>("pyc.emit.structural")) {
      llvm::StringRef v = structural.getValue();
      if (v.equals_insensitive("true") || v == "1")
        return;
    }
    if (smallPartitionThreshold > 0) {
      for (Block &b : f.getBody())
        fuseBlockByPartition(b);
      return;
    }
    for (Block &b : f.getBody())
      fuseBlock(b);
  }

  uint64_t smallPartitionThreshold = 0;

  // Scratch state for the ESSENT-partition mode (per function).
  llvm::SmallVector<Operation *> ops;
  llvm::SmallVector<llvm::SmallVector<unsigned>> preds, succs;
  llvm::SmallVector<std::pair<unsigned, unsigned>> forbidden;
  llvm::SmallVector<int> parent;
  llvm::SmallVector<uint64_t> usize;
  llvm::SmallVector<int> coneOf;
  llvm::SmallVector<llvm::SmallVector<unsigned>> cones;
  unsigned partCount = 0;

  void fuseBlock(Block &block) {
    llvm::SmallVector<Operation *> run;

    auto flushRun = [&]() {
      if (run.size() < 2) {
        run.clear();
        return;
      }
      fuseRun(run);
      run.clear();
    };

    for (Operation &op : llvm::make_early_inc_range(block)) {
      if (isFusableCombOp(&op)) {
        run.push_back(&op);
        continue;
      }
      flushRun();
    }
    flushRun();
  }

  // ---------------------------------------------------------------------------
  // ESSENT-partition mode: build the comb-op dependency graph, partition it
  // with the acyclic algorithm (Beamer & Donofrio, DAC 2020, Section IV),
  // then emit one pyc.comb region per partition.
  //
  // Sequential scheduled units (instances, fifos, memories, CDC syncs)
  // update their outputs mid-pass, so a producer feeding such a unit and a
  // consumer reading its result must never share a partition: the unit's
  // eval must run between them. These ordering barriers are modeled as
  // forbidden pairs respected by the cone growth and every merge.
  // ---------------------------------------------------------------------------

  static bool isSequentialBarrier(Operation *op) {
    return isa<pyc::InstanceOp, pyc::FifoOp, pyc::ByteMemOp, pyc::SyncMemOp,
               pyc::SyncMemDPOp, pyc::AsyncFifoOp, pyc::CdcSyncOp>(op);
  }

  void fuseBlockByPartition(Block &block) {
    // 1. Collect fusible ops (block order == topo order for SSA values).
    for (Operation &op : block)
      if (isFusableCombOp(&op))
        ops.push_back(&op);
    if (ops.size() < 2)
      return;

    llvm::DenseMap<Operation *, unsigned> idx;
    for (auto [i, op] : llvm::enumerate(ops))
      idx[op] = static_cast<unsigned>(i);
    const unsigned n = ops.size();

    preds.assign(n, {});
    succs.assign(n, {});
    for (unsigned v = 0; v < n; ++v) {
      for (Value operand : ops[v]->getOperands()) {
        Operation *def = operand.getDefiningOp();
        if (!def)
          continue;
        auto it = idx.find(def);
        if (it == idx.end())
          continue;
        preds[v].push_back(it->second);
        succs[it->second].push_back(v);
      }
    }

    // 2. Ordering barriers: producer(a) -> barrier -> consumer(b) forbids
    // partition(a) == partition(b).
    llvm::SmallVector<std::pair<unsigned, unsigned>> forbidden;
    for (Operation &op : block) {
      if (!isSequentialBarrier(&op))
        continue;
      llvm::SmallVector<unsigned> producers, consumers;
      for (Value operand : op.getOperands()) {
        Operation *def = operand.getDefiningOp();
        if (!def)
          continue;
        auto it = idx.find(def);
        if (it != idx.end())
          producers.push_back(it->second);
      }
      for (Value r : op.getResults())
        for (OpOperand &use : r.getUses()) {
          auto it = idx.find(use.getOwner());
          if (it != idx.end())
            consumers.push_back(it->second);
        }
      for (unsigned a : producers)
        for (unsigned b : consumers)
          if (a != b)
            forbidden.emplace_back(a, b);
    }
    auto mergeLegal = [&](unsigned a, unsigned b) {
      for (auto &[x, y] : forbidden) {
        unsigned rx = findRoot(x), ry = findRoot(y);
        if (rx == ry)
          continue;
        unsigned pa = findRoot(a), pb = findRoot(b);
        if ((rx == pa && ry == pb) || (rx == pb && ry == pa))
          return false;
      }
      return true;
    };

    // Union-find over ops.
    parent.resize(n);
    usize.assign(n, 1);
    for (unsigned i = 0; i < n; ++i)
      parent[i] = static_cast<int>(i);
    partCount = n;
    cones.clear();
    coneOf.assign(n, -1);

    // 3. MFFC bootstrap (reverse order so sinks seed their cones first).
    for (unsigned vi = n; vi-- > 0;) {
      unsigned v = vi;
      if (coneOf[v] >= 0)
        continue;
      int id = static_cast<int>(cones.size());
      cones.emplace_back();
      llvm::SmallVector<unsigned, 16> work{v};
      coneOf[v] = id;
      cones[id].push_back(v);
      while (!work.empty()) {
        unsigned u = work.pop_back_val();
        for (unsigned p : preds[u]) {
          if (coneOf[p] >= 0)
            continue;
          bool allInside = true;
          for (unsigned sc : succs[p])
            if (coneOf[sc] != id) {
              allInside = false;
              break;
            }
          if (!allInside)
            continue;
          if (!coneAllows(p, id))
            continue;
          coneOf[p] = id;
          cones[id].push_back(p);
          work.push_back(p);
        }
      }
    }

    // 4. Merge phases (paper Section IV). External-path legality test plus
    // ordering-barrier check before every merge.
    // Phase A: single-parent partitions merge into their parent.
    bool progressed = true;
    while (progressed && partCount > 1) {
      progressed = false;
      auto inputs = buildInputPartitions();
      for (auto &[p, ins] : inputs) {
        if (ins.size() != 1 || findRoot(p) != p)
          continue;
        int q = *ins.begin();
        if (findRoot(q) != q)
          continue;
        unionPartitions(p, q); // always safe (paper Figure 4A)
        progressed = true;
        if (partCount <= 1)
          break;
      }
    }
    // Phases B/C: small + small sibling (by cut edges), then small + any
    // sibling (by common-input fraction).
    for (int phase = 0; phase < 2 && partCount > 1; ++phase) {
      bool anyMerge = true;
      while (anyMerge && partCount > 1) {
        anyMerge = false;
        auto adj = buildAdjacency();
        auto inputs = buildInputPartitions();
        for (unsigned v = 0; v < n && partCount > 1; ++v) {
          int p = findRoot(v);
          if (p != static_cast<int>(v) || !isSmallPartition(p))
            continue;
          auto it = adj.find(p);
          if (it == adj.end())
            continue;
          int best = -1;
          uint64_t bestCut = 0;
          double bestCommon = -1.0;
          for (auto &[q, cut] : it->second) {
            int rq = findRoot(q);
            if (rq == p)
              continue;
            if (phase == 0 && !isSmallPartition(rq))
              continue;
            if (!mergeLegal(p, rq))
              continue;
            double common = commonInputFraction(p, rq, inputs);
            bool better = phase == 0
                              ? (cut > bestCut || (cut == bestCut && rq < best))
                              : (common > bestCommon ||
                                 (common == bestCommon && cut > bestCut));
            if (better) {
              best = rq;
              bestCut = cut;
              bestCommon = common;
            }
          }
          if (best >= 0) {
            unionPartitions(p, best);
            anyMerge = true;
            break;
          }
        }
      }
    }

    // 5. Guard: the partition graph must be acyclic; collapse residual SCCs
    // (forced barrier unions can only shrink partitions, but stay safe).
    collapseResidualCycles();

    // 6. Emit one region per multi-op partition.
    llvm::DenseMap<int, llvm::SmallVector<Operation *>> groups;
    for (unsigned v = 0; v < n; ++v)
      groups[findRoot(v)].push_back(ops[v]);
    for (auto &entry : groups)
      if (entry.second.size() >= 2)
        buildRegionFromMembers(entry.second);
  }

  int findRoot(unsigned v) {
    int x = static_cast<int>(v);
    while (parent[x] != x) {
      parent[x] = parent[parent[x]];
      x = parent[x];
    }
    return x;
  }

  void unionPartitions(int a, int b) {
    int ra = findRoot(static_cast<unsigned>(a));
    int rb = findRoot(static_cast<unsigned>(b));
    if (ra == rb)
      return;
    if (usize[static_cast<unsigned>(ra)] < usize[static_cast<unsigned>(rb)])
      std::swap(ra, rb);
    parent[rb] = ra;
    usize[static_cast<unsigned>(ra)] += usize[static_cast<unsigned>(rb)];
    --partCount;
  }

  bool coneAllows(unsigned p, int coneId) {
    for (auto &[x, y] : forbidden) {
      if (x == p && coneOf[y] == coneId)
        return false;
      if (y == p && coneOf[x] == coneId)
        return false;
    }
    return true;
  }

  bool isSmallPartition(int root) {
    return smallPartitionThreshold > 0 &&
           usize[static_cast<unsigned>(root)] < smallPartitionThreshold;
  }

  llvm::DenseMap<int, llvm::DenseSet<int>> buildInputPartitions() {
    llvm::DenseMap<int, llvm::DenseSet<int>> inputs;
    for (unsigned v = 0; v < ops.size(); ++v) {
      int rv = findRoot(v);
      for (unsigned p : preds[v]) {
        int rp = findRoot(p);
        if (rp != rv)
          inputs[rv].insert(rp);
      }
    }
    return inputs;
  }

  llvm::DenseMap<int, llvm::DenseMap<int, unsigned>> buildAdjacency() {
    llvm::DenseMap<int, llvm::DenseMap<int, unsigned>> adj;
    for (unsigned v = 0; v < ops.size(); ++v) {
      int rv = findRoot(v);
      for (unsigned p : preds[v]) {
        int rp = findRoot(p);
        if (rp == rv)
          continue;
        ++adj[rv][rp];
        ++adj[rp][rv];
      }
    }
    return adj;
  }

  double commonInputFraction(int p, int q,
                             llvm::DenseMap<int, llvm::DenseSet<int>> &inputs) {
    auto ip = inputs.find(p);
    auto iq = inputs.find(q);
    if (ip == inputs.end() || iq == inputs.end())
      return 0.0;
    unsigned shared = 0;
    for (int x : ip->second)
      if (iq->second.contains(x))
        ++shared;
    unsigned united = ip->second.size() + iq->second.size() - shared;
    return united > 0 ? static_cast<double>(shared) / united : 0.0;
  }

  void collapseResidualCycles() {
    while (true) {
      llvm::DenseMap<int, llvm::DenseSet<int>> succ;
      for (unsigned v = 0; v < ops.size(); ++v) {
        int rv = findRoot(v);
        for (unsigned p : preds[v]) {
          int rp = findRoot(p);
          if (rp != rv)
            succ[rp].insert(rv);
        }
      }
      llvm::SmallVector<unsigned> indeg;
      indeg.resize(ops.size());
      bool any = false;
      for (auto &entry : succ)
        for (int to : entry.second)
          ++indeg[static_cast<unsigned>(to)];
      llvm::SmallVector<int> ready;
      for (auto &entry : succ)
        if (indeg[static_cast<unsigned>(entry.first)] == 0)
          ready.push_back(entry.first);
      unsigned visited = 0;
      unsigned total = succ.size();
      while (!ready.empty()) {
        int p = ready.pop_back_val();
        ++visited;
        for (int to : succ[p])
          if (--indeg[static_cast<unsigned>(to)] == 0)
            ready.push_back(to);
      }
      if (visited == total)
        return; // acyclic
      // Union every multi-member SCC into its lowest id (iterative Tarjan).
      enum : unsigned char { kUnvisited = 0, kOpen = 1, kDone = 2 };
      llvm::SmallVector<unsigned char> state;
      state.resize(ops.size());
      llvm::SmallVector<unsigned> index, low;
      llvm::SmallVector<int> onStack;
      index.resize(ops.size());
      low.resize(ops.size());
      onStack.resize(ops.size());
      std::vector<int> tstack;
      std::vector<int> frames;
      std::vector<unsigned> frameNext(ops.size(), 0u);
      unsigned nextIndex = 0;
      bool unioned = false;
      for (unsigned root = 0; root < ops.size(); ++root) {
        if (findRoot(root) != static_cast<int>(root) ||
            state[static_cast<unsigned>(root)] != kUnvisited)
          continue;
        int r = static_cast<int>(root);
        frames.push_back(r);
        state[static_cast<unsigned>(r)] = kOpen;
        index[static_cast<unsigned>(r)] = low[static_cast<unsigned>(r)] =
            nextIndex++;
        tstack.push_back(r);
        onStack[static_cast<unsigned>(r)] = 1;
        while (!frames.empty()) {
          int v = frames.back();
          if (frameNext[static_cast<unsigned>(v)] < succ[v].size()) {
            auto it = succ[v].begin();
            std::advance(it, frameNext[static_cast<unsigned>(v)]++);
            int w = *it;
            if (state[static_cast<unsigned>(w)] == kUnvisited) {
              state[static_cast<unsigned>(w)] = kOpen;
              index[static_cast<unsigned>(w)] =
                  low[static_cast<unsigned>(w)] = nextIndex++;
              tstack.push_back(w);
              onStack[static_cast<unsigned>(w)] = 1;
              frames.push_back(w);
            } else if (onStack[static_cast<unsigned>(w)]) {
              low[static_cast<unsigned>(v)] =
                  std::min(low[static_cast<unsigned>(v)],
                           index[static_cast<unsigned>(w)]);
            }
            continue;
          }
          frames.pop_back();
          if (low[static_cast<unsigned>(v)] == index[static_cast<unsigned>(v)]) {
            std::vector<int> members;
            int w;
            do {
              w = tstack.back();
              tstack.pop_back();
              onStack[static_cast<unsigned>(w)] = 0;
              members.push_back(w);
            } while (w != v);
            if (members.size() > 1) {
              int keep = *std::min_element(members.begin(), members.end());
              for (int m : members) {
                if (m == keep)
                  continue;
                usize[static_cast<unsigned>(keep)] +=
                    usize[static_cast<unsigned>(m)];
                parent[m] = keep;
                --partCount;
                unioned = true;
              }
            }
          }
          if (!frames.empty()) {
            int parentF = frames.back();
            low[static_cast<unsigned>(parentF)] =
                std::min(low[static_cast<unsigned>(parentF)],
                         low[static_cast<unsigned>(v)]);
          }
          state[static_cast<unsigned>(v)] = kDone;
        }
      }
      if (!unioned)
        return;
    }
  }

  void fuseRun(ArrayRef<Operation *> run) {
    buildRegionFromMembers(run);
  }

  /// Builds one pyc.comb region around `members` (a set of fusible ops in
  /// block order). Shared by the textual and ESSENT-partition modes.
  void buildRegionFromMembers(ArrayRef<Operation *> members) {
    ArrayRef<Operation *> run = members;
    llvm::DenseSet<Operation *> runSet;
    runSet.reserve(run.size());
    for (Operation *op : run)
      runSet.insert(op);

    // Live-outs: results that are used by an op outside the run.
    llvm::SmallVector<Value> outputs;
    llvm::DenseSet<Value> outputSet;
    for (Operation *op : run) {
      for (Value r : op->getResults()) {
        bool usedOutside = false;
        for (OpOperand &use : r.getUses()) {
          if (!runSet.contains(use.getOwner())) {
            usedOutside = true;
            break;
          }
        }
        if (usedOutside && outputSet.insert(r).second)
          outputs.push_back(r);
      }
    }

    // Promote any pyc.lazy_probe_wires host in the run to a live-out so
    // its wired result survives as a comb output. Observation demand
    // hangs the attribute on the alias-stripped producer (see
    // findWireHost in ApplyObservationDemandPass), so a named alias that
    // is the live-out, or a mid-pipeline named op consumed only by other
    // fused ops, would otherwise be cloned into the pyc.comb body and
    // become invisible to the emitter (which skips ops inside pyc.comb).
    for (Operation *op : run) {
      auto wires = op->getAttrOfType<ArrayAttr>("pyc.lazy_probe_wires");
      if (!wires)
        continue;
      for (Attribute item : wires) {
        auto dict = dyn_cast<DictionaryAttr>(item);
        if (!dict)
          continue;
        auto resultAttr = dict.getAs<IntegerAttr>("result");
        const unsigned srcResult =
            resultAttr ? static_cast<unsigned>(resultAttr.getInt()) : 0;
        if (srcResult >= op->getNumResults())
          continue;
        Value r = op->getResult(srcResult);
        if (outputSet.insert(r).second)
          outputs.push_back(r);
      }
    }

    if (outputs.empty())
      return;

    // External inputs: operands that are not defined by an op in the run.
    llvm::SmallVector<Value> inputs;
    llvm::DenseSet<Value> seenInputs;
    for (Operation *op : run) {
      for (Value v : op->getOperands()) {
        if (Operation *def = v.getDefiningOp()) {
          if (runSet.contains(def))
            continue;
        }
        if (seenInputs.insert(v).second)
          inputs.push_back(v);
      }
    }

    llvm::SmallVector<Type> outTypes;
    outTypes.reserve(outputs.size());
    for (Value v : outputs)
      outTypes.push_back(v.getType());

    OpBuilder builder(run.front());
    auto comb = builder.create<pyc::CombOp>(run.front()->getLoc(), outTypes, inputs);

    // Build a single-block region and clone ops into it.
    Region &region = comb.getBody();
    Block *body = new Block();
    region.push_back(body);
    for (Value in : inputs)
      body->addArgument(in.getType(), comb.getLoc());

    IRMapping mapping;
    for (auto [i, in] : llvm::enumerate(inputs))
      mapping.map(in, body->getArgument(i));

    builder.setInsertionPointToStart(body);
    for (Operation *op : run) {
      Operation *cloned = builder.clone(*op, mapping);
      for (auto [oldRes, newRes] : llvm::zip(op->getResults(), cloned->getResults()))
        mapping.map(oldRes, newRes);
    }

    llvm::SmallVector<Value> yieldVals;
    yieldVals.reserve(outputs.size());
    for (Value out : outputs)
      yieldVals.push_back(mapping.lookup(out));
    builder.create<pyc::YieldOp>(comb.getLoc(), yieldVals);

    // Keep undeclared combinational names lookupable after fusion: they
    // register as addWire of this comb result, not a second named copy.
    SmallVector<Attribute> lazyWires;
    auto copyLazyWires = [&](Value out, int64_t combResult) {
      Operation *def = out.getDefiningOp();
      if (!def)
        return;
      auto wires = def->getAttrOfType<ArrayAttr>("pyc.lazy_probe_wires");
      if (!wires)
        return;
      unsigned outResult = 0;
      if (auto res = dyn_cast<OpResult>(out))
        outResult = res.getResultNumber();
      for (Attribute item : wires) {
        auto dict = dyn_cast<DictionaryAttr>(item);
        if (!dict)
          continue;
        auto resultAttr = dict.getAs<IntegerAttr>("result");
        const unsigned srcResult =
            resultAttr ? static_cast<unsigned>(resultAttr.getInt()) : 0;
        if (srcResult != outResult)
          continue;
        NamedAttrList fields;
        if (auto name = dict.getAs<StringAttr>("name"))
          fields.set("name", name);
        if (auto width = dict.getAs<IntegerAttr>("width"))
          fields.set("width", width);
        fields.set("result", builder.getI64IntegerAttr(combResult));
        lazyWires.push_back(DictionaryAttr::get(def->getContext(), fields));
      }
    };
    for (auto [i, out] : llvm::enumerate(outputs))
      copyLazyWires(out, static_cast<int64_t>(i));
    if (!lazyWires.empty())
      comb->setAttr("pyc.lazy_probe_wires",
                    ArrayAttr::get(comb.getContext(), lazyWires));

    // Replace uses outside the run with comb results.
    for (auto [out, res] : llvm::zip(outputs, comb.getResults())) {
      out.replaceUsesWithIf(res, [&](OpOperand &use) { return !runSet.contains(use.getOwner()); });
    }

    for (Operation *op : llvm::reverse(run))
      op->erase();
  }
};

} // namespace

std::unique_ptr<mlir::Pass> createFuseCombPass() { return std::make_unique<FuseCombPass>(); }

std::unique_ptr<mlir::Pass>
createFuseCombPass(uint64_t smallPartitionThreshold) {
  return std::make_unique<FuseCombPass>(smallPartitionThreshold);
}

static PassRegistration<FuseCombPass> pass;

} // namespace pyc
