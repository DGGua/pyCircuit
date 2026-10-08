#pragma once

#include "pyc/Simulation/SimGraph.h"

namespace pyc {

struct SimGraphPassOptions {
  bool enableCombFusion = true;
  bool enableGroupActivation = true;
  bool enableUsedBitActivation = true;
  bool enableExpressionInlining = true;
  bool enableReplication = true;
  // Default GSIM partitioning keeps coarsened supernodes indivisible. The
  // legacy strict bound remains available for targeted partition experiments.
  bool strictSupernodeBound = false;
  unsigned supernodeMaxSize = 35;
};

// Apply graph-local C++ execution optimizations. The result is still a
// SimGraph; no legalized PYC operations are rewritten.
mlir::LogicalResult runSimGraphPasses(SimGraph &graph,
                                      const SimGraphPassOptions &options = {});

} // namespace pyc
