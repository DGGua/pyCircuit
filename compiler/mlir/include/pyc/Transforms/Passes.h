#pragma once

#include <memory>
#include <string>

#include "mlir/Pass/Pass.h"

namespace pyc {

enum class DelayChainMode {
  Generated,
  Structural,
};

std::unique_ptr<::mlir::Pass> createCombCanonicalizePass();
std::unique_ptr<::mlir::Pass> createInlineFunctionsPass();
std::unique_ptr<::mlir::Pass> createFuseCombPass();
/// Reject pyc.comb bodies that are not safe for input-change memoization.
std::unique_ptr<::mlir::Pass> createCheckCombMemoizablePass();
/// Build the function-level CombDepGraph and directly materialize final
/// runtime scheduling units as sibling pyc.comb operations.
/// A value of zero disables partitioning.
std::unique_ptr<::mlir::Pass> createPartitionCombPass(unsigned maxNodes = 35);
/// Independently split each top-level fused pyc.comb into placement-free local
/// DAG partitions. Already partitioned combs are left unchanged.
/// A value of zero disables partitioning.
std::unique_ptr<::mlir::Pass>
createPartitionFusedCombPass(unsigned maxNodes = 35);
/// Verify the structural metadata and forward-only dependencies produced by
/// createPartitionCombPass().
std::unique_ptr<::mlir::Pass> createCheckCombPartitionsPass();
std::unique_ptr<::mlir::Pass> createEliminateWiresPass();
std::unique_ptr<::mlir::Pass> createAnalyzeStateOptimizationPass();
std::unique_ptr<::mlir::Pass> createAnalyzeRetimingPass();
std::unique_ptr<::mlir::Pass> createStripStateObservabilityPass();
std::unique_ptr<::mlir::Pass>
createRetimePipelinesPass(unsigned maxStages = 0,
                          unsigned maxExtraCombOps = 32,
                          unsigned maxCombDepth = 32,
                          bool preserveObservability = false,
                          bool accumulateStats = false);
std::unique_ptr<::mlir::Pass>
createCombineDelayChainsPass(DelayChainMode mode = DelayChainMode::Generated,
                             bool accumulateStats = false,
                             bool cascadeRound = false,
                             bool preserveObservability = false,
                             bool mergeOnly = false,
                             bool skipMerge = false);
std::unique_ptr<::mlir::Pass>
createPackStateLanesPass(unsigned maxWidth = 192,
                         bool preserveObservability = false);
std::unique_ptr<::mlir::Pass> createPackI1RegsPass();
std::unique_ptr<::mlir::Pass> createLowerSCFToPYCStaticPass();
std::unique_ptr<::mlir::Pass> createCheckFrontendContractPass();
std::unique_ptr<::mlir::Pass> createCheckHierarchyDisciplinePass();
std::unique_ptr<::mlir::Pass> createCheckNoDynamicPass();
/// Enforce the current single-driver pyc.wire contract before wire
/// elimination.  Observable/read wires require exactly one pyc.assign driver;
/// multiple drivers are illegal until an explicit resolved-net op exists.
std::unique_ptr<::mlir::Pass> createCheckWireDriversPass();
std::unique_ptr<::mlir::Pass> createCheckCombCyclesPass();
std::unique_ptr<::mlir::Pass> createCheckClockDomainsPass();
std::unique_ptr<::mlir::Pass> createCheckFlatTypesPass();
std::unique_ptr<::mlir::Pass> createPrunePortsPass();
std::unique_ptr<::mlir::Pass> createEliminateDeadStatePass();
std::unique_ptr<::mlir::Pass> createEliminateDeadInstancesPass();
std::unique_ptr<::mlir::Pass> createSLPPackWiresPass();
std::unique_ptr<::mlir::Pass> createCheckLogicDepthPass(unsigned logicDepth);
std::unique_ptr<::mlir::Pass> createCollectCompileStatsPass();
std::unique_ptr<::mlir::Pass> createFlattenInstancesPass();
/// C++ emit prep: sets module comb chunk size and runs member placement.
/// The optional trace codegen plan pins selected named values to stable
/// struct storage so generated VCD registration can sample them.
std::unique_ptr<::mlir::Pass>
createCppPlacementPass(unsigned combChunkNodes,
                       std::string traceCodegenPlanPath = {});
std::unique_ptr<::mlir::Pass> createVectorUnrollPass();

} // namespace pyc
