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
#include <mutex>
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
  // Single-driver wire results -> their unique driving pyc.assign. Wires
  // (and their assigns) stay outside the regions; the partition graph and
  // region inputs simply route through them transparently.
  llvm::DenseMap<Value, Operation *> wireDriverOf;

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
    // The ESSENT partition scratch lives in pass members. MLIR may execute
    // function passes for different functions concurrently (and pass clones
    // cannot always be assumed), so serialize graph-mode partitioning.
    static std::mutex partitionMu;
    std::lock_guard<std::mutex> partitionLock(partitionMu);
    // Reset scratch state: the pass instance is reused across blocks AND
    // (when MLIR threading is off) across functions; stale entries would be
    // dangling Operation* from previous fusions.
    ops.clear();
    preds.clear();
    succs.clear();
    forbidden.clear();
    cones.clear();
    wireDriverOf.clear();
    {
      llvm::DenseMap<Value, llvm::SmallVector<Operation *>> wireWriters;
      for (Operation &op : block)
        if (auto a = dyn_cast<pyc::AssignOp>(op))
          wireWriters[a.getDst()].push_back(&op);
      for (auto &[dst, writers] : wireWriters)
        if (writers.size() == 1)
          if (Operation *wireOp = dyn_cast_or_null<pyc::WireOp>(dst.getDefiningOp()))
            wireDriverOf[dst] = writers.front();
    }
    // 1. Collect graph nodes: fusible comb ops (emitted into regions) plus
    // single-driver wire results as pseudo-nodes (never emitted; they let
    // MFFC cones cross the declare-early/assign-late boundary so a packer
    // like concat merges with its upstream producer cone).
    for (Operation &op : block)
      if (isFusableCombOp(&op))
        ops.push_back(&op);
    const unsigned combOpCount = ops.size();
    if (combOpCount < 2)
      return;
    // Wire pseudo-nodes are keyed by wire op; remember their index space.
    llvm::DenseMap<Value, unsigned> wireNodeOf;
    for (auto &[wireRes, assign] : wireDriverOf)
      wireNodeOf[wireRes] = static_cast<unsigned>(ops.size()),
      ops.push_back(wireRes.getDefiningOp());
    const bool hasWireNodes = ops.size() > combOpCount;

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
        // Naming aliases are transparent (pure renames kept outside the
        // regions); route the dependency to their operand.
        for (unsigned hop = 0;
             isa_and_nonnull<pyc::AliasOp>(def) && !idx.count(def) &&
             def->getNumOperands() > 0 && hop < 64;
             ++hop) {
          Operation *next = def->getOperand(0).getDefiningOp();
          if (!next || next == def)
            break;
          def = next;
        }
        auto it = idx.find(def);
        if (it == idx.end())
          continue;
        preds[v].push_back(it->second);
        succs[it->second].push_back(v);
      }
    }
    // Wire pseudo-node edges: producer -> wire -> readers. The producer is
    // the driving assign's source; readers are all candidate ops using the
    // wire value. A wire only joins a cone when ALL its candidate readers
    // are inside (standard MFFC admission), so a wire read outside the
    // candidate set (return/probe/instance operand) keeps the boundary.
    if (hasWireNodes) {
      for (auto &[wireRes, assign] : wireDriverOf) {
        unsigned w = wireNodeOf[wireRes];
        Value src = assign->getOperand(1);
        if (Operation *srcDef = src.getDefiningOp()) {
          Operation *def = srcDef;
          for (unsigned hop = 0;
               isa_and_nonnull<pyc::AliasOp>(def) && !idx.count(def) &&
               def->getNumOperands() > 0 && hop < 64;
               ++hop) {
            Operation *next = def->getOperand(0).getDefiningOp();
            if (!next || next == def)
              break;
            def = next;
          }
          auto it = idx.find(def);
          if (it != idx.end()) {
            preds[w].push_back(it->second);
            succs[it->second].push_back(w);
          }
        }
        for (OpOperand &use : wireRes.getUses()) {
          auto it = idx.find(use.getOwner());
          if (it == idx.end() || it->second == w)
            continue;
          preds[it->second].push_back(w);
          succs[w].push_back(it->second);
        }
      }
    }

    // 2. Ordering barriers: producer(a) -> barrier -> consumer(b) forbids
    // Precompute per-register driver/reader op sets (linear passes).
    llvm::DenseMap<Operation *, llvm::SmallVector<unsigned>> regDrivers, regReaders;
    for (unsigned v = 0; v < n; ++v) {
      for (Value operand : ops[v]->getOperands()) {
        Operation *def = operand.getDefiningOp();
        if (!def || !isa<pyc::RegOp, pyc::DelayLineOp>(def))
          continue;
        if (operand == def->getResult(0)) // reading `q`
          regReaders[def].push_back(v);
      }
    }
    // Drivers: the defining op of the register's `next` operand (nothing
    // else consumes `next`; the register itself is not a graph node), with
    // wire/alias transparency so a `next` fed through a lifted wire still
    // constrains the wire's source producer.
    for (Operation &reg : block) {
      if (!isa<pyc::RegOp, pyc::DelayLineOp>(reg))
        continue;
      Value next = reg.getOperand(3);
      Operation *def = next.getDefiningOp();
      for (unsigned hop = 0; hop < 64; ++hop) {
        if (auto assignOp = dyn_cast_or_null<pyc::AssignOp>(def)) {
          // next fed by a wire: the driver is the assign's source.
          Value src = assignOp->getOperand(1);
          def = src.getDefiningOp();
          continue;
        }
        if (isa_and_nonnull<pyc::AliasOp>(def) && def->getNumOperands() > 0) {
          Operation *nxt = def->getOperand(0).getDefiningOp();
          if (!nxt || nxt == def)
            break;
          def = nxt;
          continue;
        }
        break;
      }
      if (auto it = idx.find(def); it != idx.end())
        regDrivers[&reg].push_back(it->second);
    }
    // 2. Ordering barriers: producer(a) -> barrier -> consumer(b) forbids
    // partition(a) == partition(b).
    llvm::SmallVector<std::pair<unsigned, unsigned>> barrierPairs;
    for (Operation &op : block) {
      // Register-like units need the constraint in BOTH directions: ops
      // feeding `next` and ops reading `q` must never share a partition
      // (a merged region driving and reading the same register is
      // unplaceable in the linear block order). Drivers/readers are
      // precomputed; this branch runs BEFORE the isSequentialBarrier
      // filter because RegOp/DelayLineOp are not in that list.
      if (isa<pyc::RegOp, pyc::DelayLineOp>(&op)) {
        // Register feedback (drivers of `next` vs readers of `q`) no
        // longer blocks partitioning: the cone-head relay wire emitted in
        // buildRegionFromMembers resolves the placement cycle, so a single
        // region may both read `q` and produce `next`.
        continue;
      }
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
            barrierPairs.emplace_back(a, b);
    }
    forbidden.clear();
    forbidden.append(barrierPairs.begin(), barrierPairs.end());
    auto mergeLegal = [&](unsigned a, unsigned b) {
      for (auto &[x, y] : barrierPairs) {
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
      fprintf(stderr, "[PA] parts=%u\n", partCount);
      for (auto &[p, ins] : inputs) {
        if (findRoot(p) != p)
          continue;
        fprintf(stderr, "[PA] part %u (size %u) inputs:", p, usize[p]);
        for (int ip : ins)
          fprintf(stderr, " %u", ip);
        fprintf(stderr, "\n");
      }
      for (auto &[p, ins] : inputs) {
        if (ins.size() != 1 || findRoot(p) != p)
          continue;
        int q = *ins.begin();
        if (findRoot(q) != q)
          continue;
        if (!mergeLegal(p, q))
          continue;
        unionPartitions(p, q);
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

    {
      fprintf(stderr, "[GR] final groups:\n");
      for (unsigned v = 0; v < n; ++v)
        fprintf(stderr, "[GR] node %u (%s) -> part %d\n", v,
                ops[v]->getName().getStringRef().str().c_str(), findRoot(v));
    }
    // 5. Guard: the partition graph must be acyclic; collapse residual SCCs
    // (forced barrier unions can only shrink partitions, but stay safe).
    collapseResidualCycles();

    // 6. Emit one region per multi-op partition. Wire pseudo-nodes join
    // partitions during analysis but are never emitted: they are removed
    // from the member list here, and buildRegionFromMembers records the
    // lifted boundary (readers route to the driver's producer clone; the
    // driving assign is rewritten to write the comb result and moved after
    // the region) via wireDriverOf.
    llvm::DenseMap<int, llvm::SmallVector<Operation *>> groups;
    for (unsigned v = 0; v < n; ++v)
      groups[findRoot(v)].push_back(ops[v]);
    for (auto &entry : groups) {
      llvm::SmallVector<Operation *> members;
      for (Operation *op : entry.second)
        if (entry.second.size() >= 2)
          if (!isa<pyc::WireOp>(op))
            members.push_back(op);
      if (members.size() >= 2)
        buildRegionFromMembers(members, /*graphMode=*/true);
    }
    // Demote lifted wires whose only remaining reader is their driving
    // assign (the consumer cone now reads the region-internal value): the
    // wire+assign pair degenerates to a named alias of the comb result,
    // which keeps the probe name addressable without the storage cell.
    for (auto &[wireRes, assign] : llvm::make_early_inc_range(wireDriverOf)) {
      bool onlyWriterReads = true;
      for (OpOperand &use : wireRes.getUses())
        if (use.getOwner() != assign) {
          onlyWriterReads = false;
          break;
        }
      if (!onlyWriterReads)
        continue;
      Value src = assign->getOperand(1);
      auto srcRes = dyn_cast<OpResult>(src);
      pyc::CombOp srcComb;
      if (srcRes)
        if (auto c = dyn_cast_or_null<pyc::CombOp>(srcRes.getDefiningOp()))
          srcComb = c;
      if (!srcComb)
        continue;
      Operation *wireOp = wireRes.getDefiningOp();
      // Fold the probe record straight into the comb's lazy_probe_wires
      // (same format copyLazyWires emits) instead of materializing a
      // top-level named alias.
      llvm::SmallVector<Attribute> records;
      if (auto existing =
              srcComb->getAttrOfType<ArrayAttr>("pyc.lazy_probe_wires"))
        records.append(existing.getValue().begin(), existing.getValue().end());
      NamedAttrList fields;
      if (auto name = wireOp->getAttrOfType<StringAttr>("pyc.name"))
        fields.set("name", name);
      if (auto width = wireOp->getAttrOfType<IntegerAttr>("pyc.width"))
        fields.set("width", width);
      else if (auto ty = dyn_cast<IntegerType>(wireRes.getType()))
        fields.set("width",
                   Builder(wireOp->getContext()).getI64IntegerAttr(ty.getWidth()));
      fields.set("result",
                 Builder(wireOp->getContext())
                     .getI64IntegerAttr(static_cast<int64_t>(
                         srcRes.getResultNumber())));
      records.push_back(
          DictionaryAttr::get(wireOp->getContext(), fields));
      srcComb->setAttr("pyc.lazy_probe_wires",
                       ArrayAttr::get(wireOp->getContext(), records));
      assign->erase();
      wireRes.replaceAllUsesWith(src);
      wireOp->erase();
      wireDriverOf.erase(wireRes);
    }
    topologicallyReorderBlock(block);
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
    buildRegionFromMembers(run, /*graphMode=*/false);
  }

  /// Builds one pyc.comb region around `members` (a set of fusible ops in
  /// block order). Shared by the textual and ESSENT-partition modes.
  void buildRegionFromMembers(ArrayRef<Operation *> members,
                             bool graphMode = false) {
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

    // External inputs (transparent through excluded aliases and single-
    // driver wires): operands routed through such boundaries resolve to
    // their ultimate producer. When that producer is a member the value is
    // region-internal (taking it as an input would create an ordering
    // cycle); otherwise it becomes a region input and the original value
    // maps onto the corresponding block argument.
    auto resolveTransparent = [&](Value v) {
      unsigned hop = 0;
      while (Operation *def = v.getDefiningOp()) {
        if (runSet.contains(def) || hop++ > 64)
          break;
        if (auto aliasOp = dyn_cast<pyc::AliasOp>(def)) {
          v = aliasOp->getOperand(0);
          continue;
        }
        if (auto wireOp = dyn_cast<pyc::WireOp>(def)) {
          auto it2 = wireDriverOf.find(wireOp->getResult(0));
          if (it2 == wireDriverOf.end())
            break;
          v = it2->second->getOperand(1);
          continue;
        }
        break;
      }
      return v;
    };
    llvm::SmallVector<Value> inputs;
    llvm::DenseMap<Value, Value> routedInternal; // base is a member result
    llvm::DenseMap<Value, Value> routedExternal; // base is a region input
    llvm::DenseSet<Value> seenInputs;
    for (Operation *op : run) {
      for (Value v : op->getOperands()) {
        if (Operation *def = v.getDefiningOp()) {
          if (runSet.contains(def))
            continue;
        }
        Value base = resolveTransparent(v);
        if (Operation *bdef = base.getDefiningOp()) {
          if (runSet.contains(bdef)) {
            if (v != base)
              routedInternal[v] = base;
            continue;
          }
        }
        if (seenInputs.insert(base).second)
          inputs.push_back(base);
        if (v != base)
          routedExternal[v] = base;
      }
    }

    llvm::SmallVector<Type> outTypes;
    outTypes.reserve(outputs.size());
    for (Value v : outputs)
      outTypes.push_back(v.getType());

    Block &regionBlock = *run.front()->getBlock();
    Operation *anchor = run.front();
    for (Value in : inputs) {
      Operation *def = in.getDefiningOp();
      if (!def || def->getBlock() != &regionBlock)
        continue;
      if (def->isBeforeInBlock(anchor))
        continue;
      anchor = def;
    }
    OpBuilder builder(anchor);
    builder.setInsertionPointAfter(anchor);
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
    // Values routed to a region input read its block argument directly;
    // ones routed to a member producer are pre-mapped to the original value
    // and redirected to the clone after cloning.
    for (auto &[v, base] : routedExternal)
      mapping.map(v, mapping.lookup(base));
    for (auto &[v, base] : routedInternal)
      mapping.map(v, base);
    builder.setInsertionPointToStart(body);
    {
      // Clone in data-dependence topological order (block order as
      // tiebreak): a member's operand may be produced by another member
      // that sits LATER in the block (values routed through lifted wires),
      // which block order cannot express.
      llvm::DenseMap<Operation *, unsigned> posOf;
      for (auto [i, op] : llvm::enumerate(run))
        posOf[op] = static_cast<unsigned>(i);
      llvm::SmallVector<Operation *> ordered(run.begin(), run.end());
      llvm::DenseMap<Operation *, llvm::SmallVector<Operation *>> memDeps;
      llvm::DenseMap<Operation *, unsigned> indeg;
      for (Operation *op : run) {
        unsigned d = 0;
        for (Value operand : op->getOperands()) {
          // Operands routed through wires resolve to the producer that the
          // clone will actually read; the dependence edge must follow the
          // resolved value, not the wire.
          Value base = resolveTransparent(operand);
          Operation *def = base.getDefiningOp();
          auto pit = posOf.find(def);
          if (pit == posOf.end() || def == op)
            continue;
          memDeps[def].push_back(op);
          ++d;
        }
        indeg[op] = d;
      }
      llvm::SmallVector<Operation *, 16> ready;
      for (Operation *op : ordered)
        if (indeg[op] == 0)
          ready.push_back(op);
      llvm::SmallVector<Operation *> emitted;
      emitted.reserve(run.size());
      llvm::DenseSet<Operation *> emittedSet;
      while (!ready.empty()) {
        // stable: earliest block position first
        unsigned best = 0;
        for (unsigned i = 1; i < ready.size(); ++i)
          if (posOf[ready[i]] < posOf[ready[best]])
            best = i;
        Operation *op = ready[best];
        ready.erase(ready.begin() + best);
        emitted.push_back(op);
        emittedSet.insert(op);
        for (Operation *dep : memDeps[op])
          if (--indeg[dep] == 0)
            ready.push_back(dep);
      }
      if (emitted.size() != run.size()) {
        // Cyclic member deps cannot happen (partition graph is acyclic);
        // fall back to block order defensively.
        emitted.assign(run.begin(), run.end());
      }
      for (Operation *op : emitted) {
        Operation *cloned = builder.clone(*op, mapping);
        for (auto [oldRes, newRes] : llvm::zip(op->getResults(), cloned->getResults()))
          mapping.map(oldRes, newRes);
      }
    }
    // Redirect operands still referencing original member results (routed
    // through wire/alias boundaries) to their clones.
    for (Operation &bodyOp : body->getOperations()) {
      for (OpOperand &operand : bodyOp.getOpOperands()) {
        Operation *def = operand.get().getDefiningOp();
        if (def && runSet.contains(def))
          operand.set(mapping.lookup(operand.get()));
        else if (routedInternal.count(operand.get()))
          operand.set(mapping.lookup(routedInternal[operand.get()]));
      }
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

    // Replace uses outside the run with comb results. A sequential unit's
    // `next` operand cannot read the comb result directly: the unit must
    // also dominate the region (its `q` feeds the region), so the direct
    // edge would close a placement cycle. Route such uses through a fresh
    // wire declared before the unit and driven by an assign after the
    // region — the declare-early/assign-late cell the frontend itself uses
    // for register feedback.
    for (auto [out, res] : llvm::zip(outputs, comb.getResults())) {
      OpBuilder::InsertionGuard guard(builder);
      for (OpOperand &use : llvm::make_early_inc_range(out.getUses())) {
        if (!graphMode || runSet.contains(use.getOwner()))
          continue;
        Operation *user = use.getOwner();
        if (!isa<pyc::RegOp, pyc::DelayLineOp>(user) ||
            user->getOperand(3) != out)
          continue;
        OpBuilder wireBuilder(user);
        auto wire = wireBuilder.create<pyc::WireOp>(user->getLoc(), out.getType());
        builder.setInsertionPointAfter(comb.getOperation());
        auto wireAssign = builder.create<pyc::AssignOp>(
            comb.getLoc(), wire.getResult(), res);
        use.set(wire.getResult());
        wireDriverOf[wire.getResult()] = wireAssign;
      }
      out.replaceUsesWithIf(res, [&](OpOperand &use) { return !runSet.contains(use.getOwner()); });
    }

    for (Operation *op : llvm::reverse(run))
      op->erase();

  }

  /// Stable topological reorder of the whole block after graph-mode
  /// emission. Lifting wire boundaries relocates producers and consumers
  /// (assigns now write comb results; sequential units read them), and any
  /// point-wise fixup cascades. One final pass sorts every op by data
  /// dependence (original block order as tiebreak), which is exactly how
  /// the emitter evaluates the design, so the result is dominance-correct
  /// and semantics-preserving: wires keep declare-early/assign-late, single
  /// drivers keep their write order, and reg/delay-line positions are
  /// schedule-neutral.
  void topologicallyReorderBlock(Block &block) {
    llvm::SmallVector<Operation *> opsInOrder;
    for (Operation &op : block)
      if (!isa<func::ReturnOp>(op))
        opsInOrder.push_back(&op);
    llvm::DenseMap<Operation *, unsigned> pos;
    for (auto [i, op] : llvm::enumerate(opsInOrder))
      pos[op] = static_cast<unsigned>(i);
    llvm::DenseMap<Operation *, llvm::SmallVector<Operation *>> succsOf;
    llvm::DenseMap<Operation *, unsigned> indeg;
    llvm::DenseSet<std::pair<Operation *, Operation *>> edgeSeen;
    auto addEdge = [&](Operation *from, Operation *to) {
      if (from == to)
        return;
      if (!edgeSeen.insert({from, to}).second)
        return;
      succsOf[from].push_back(to);
      indeg[to] += 1;
    };
    for (Operation *op : opsInOrder) {
      indeg.try_emplace(op, 0u);
      if (isa<pyc::WireOp>(op))
        continue; // declaration: deps linked via assign below
      if (auto a = dyn_cast<pyc::AssignOp>(op)) {
        if (Operation *src = a.getSrc().getDefiningOp())
          if (pos.count(src))
            addEdge(src, op);
        if (Operation *dstOp = a.getDst().getDefiningOp()) {
          if (pos.count(dstOp) && isa<pyc::WireOp>(dstOp))
            addEdge(dstOp, op); // wire declared before its driver
          for (OpOperand &use : dstOp->getResult(0).getUses())
            if (use.getOwner() != op && pos.count(use.getOwner()))
              addEdge(op, use.getOwner()); // readers follow the write
        }
        continue;
      }
      if (isa<pyc::RegOp, pyc::DelayLineOp>(op)) {
        // Sequential units are evaluation sources: `q` holds the previous
        // commit and `next` is only sampled at commit time, so they never
        // wait on same-tick producers (the emitter gives them their own
        // slots). Their operands impose no ordering edges here.
        continue;
      }
      for (Value operand : op->getOperands()) {
        if (Operation *def = operand.getDefiningOp()) {
          if (pos.count(def))
            addEdge(def, op);
          if (isa<pyc::WireOp>(def)) {
            auto it = wireDriverOf.find(operand);
            if (it != wireDriverOf.end() && pos.count(it->second))
              addEdge(it->second, op);
          }
        }
      }
    }
    llvm::SmallVector<Operation *, 32> ready;
    for (Operation *op : opsInOrder)
      if (indeg[op] == 0)
        ready.push_back(op);
    llvm::SmallVector<Operation *> out;
    out.reserve(opsInOrder.size());
    while (!ready.empty()) {
      unsigned best = 0;
      for (unsigned i = 1; i < ready.size(); ++i)
        if (pos[ready[i]] < pos[ready[best]])
          best = i;
      Operation *op = ready[best];
      ready.erase(ready.begin() + best);
      out.push_back(op);
      auto it = succsOf.find(op);
      if (it == succsOf.end())
        continue;
      for (Operation *nxt : it->second)
        if (--indeg[nxt] == 0)
          ready.push_back(nxt);
    }
    if (out.size() != opsInOrder.size()) {
      llvm::DenseSet<Operation *> emitted(out.begin(), out.end());
      for (Operation *op : opsInOrder)
        if (!emitted.contains(op))
          fprintf(stderr, "[REORDER] stuck %s (indeg=%u)\n",
                  op->getName().getStringRef().str().c_str(), indeg[op]);
    }
    if (out.size() != opsInOrder.size())
      return; // cyclic (should not happen); keep current order
    Operation *cursor = &block.front();
    for (Operation *op : out) {
      op->moveAfter(cursor);
      cursor = op;
    }
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
