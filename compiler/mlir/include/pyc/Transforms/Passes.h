#pragma once

#include <memory>

#include "mlir/Pass/Pass.h"

namespace pyc {

enum class DelayChainMode {
  Generated,
  Structural,
};

std::unique_ptr<::mlir::Pass> createCombCanonicalizePass();
std::unique_ptr<::mlir::Pass> createInlineFunctionsPass();
std::unique_ptr<::mlir::Pass> createFuseCombPass();
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
std::unique_ptr<::mlir::Pass> createCheckFlatTypesPass();
std::unique_ptr<::mlir::Pass> createPrunePortsPass();
std::unique_ptr<::mlir::Pass> createEliminateDeadStatePass();
std::unique_ptr<::mlir::Pass> createEliminateDeadInstancesPass();
std::unique_ptr<::mlir::Pass> createSLPPackWiresPass();
std::unique_ptr<::mlir::Pass> createCheckLogicDepthPass(unsigned logicDepth);
std::unique_ptr<::mlir::Pass> createCollectCompileStatsPass();
std::unique_ptr<::mlir::Pass> createFlattenInstancesPass();
/// C++ emit prep: sets module comb chunk size and runs member placement.
std::unique_ptr<::mlir::Pass> createCppPlacementPass(unsigned combChunkNodes);
std::unique_ptr<::mlir::Pass> createVectorUnrollPass();

} // namespace pyc
