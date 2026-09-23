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
/// Reject pyc.comb bodies that are not deterministic and exactly comparable.
std::unique_ptr<::mlir::Pass> createCheckCombMemoizablePass();
/// Materialize stable GSIM-style sibling pyc.comb partitions. Zero disables it.
std::unique_ptr<::mlir::Pass> createPartitionCombPass(unsigned maxNodes = 35);
/// Verify partition metadata, direct boundaries, and forward dependencies.
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
std::unique_ptr<::mlir::Pass> createCheckCombCyclesPass();
std::unique_ptr<::mlir::Pass> createCheckClockDomainsPass();
/// Analyze the module stage DAG, optionally materializing supported false SCCs.
std::unique_ptr<::mlir::Pass> createModulePipelinePass(bool rewrite = false);
std::unique_ptr<::mlir::Pass> createPlanChangeDrivenSchedulePass();
std::unique_ptr<::mlir::Pass> createCheckChangeDrivenSchedulePass();
std::unique_ptr<::mlir::Pass> createCheckFlatTypesPass();
std::unique_ptr<::mlir::Pass> createPrunePortsPass();
std::unique_ptr<::mlir::Pass> createEliminateDeadStatePass();
std::unique_ptr<::mlir::Pass> createEliminateDeadInstancesPass();
std::unique_ptr<::mlir::Pass> createSLPPackWiresPass();
std::unique_ptr<::mlir::Pass> createCheckLogicDepthPass(unsigned logicDepth);
std::unique_ptr<::mlir::Pass> createCollectCompileStatsPass();
std::unique_ptr<::mlir::Pass> createFlattenInstancesPass();
/// C++ emit prep: sets module comb chunk size and runs member placement.
/// The optional trace codegen plan validates selected named fields and keeps
/// them on stable struct storage so generated VCD registration can sample them.
std::unique_ptr<::mlir::Pass>
createCppPlacementPass(unsigned combChunkNodes,
                       std::string traceCodegenPlanPath = {});
std::unique_ptr<::mlir::Pass> createVectorUnrollPass();

} // namespace pyc
