#pragma once

#include <memory>
#include <string>

#include "mlir/Dialect/Func/IR/FuncOps.h"
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
std::unique_ptr<::mlir::Pass>
createRetimePipelinesPass(unsigned maxStages = 0,
                          unsigned maxExtraCombOps = 32,
                          unsigned maxCombDepth = 32,
                          bool accumulateStats = false);
std::unique_ptr<::mlir::Pass>
createCombineDelayChainsPass(DelayChainMode mode = DelayChainMode::Generated,
                             bool accumulateStats = false,
                             bool cascadeRound = false,
                             bool mergeOnly = false,
                             bool skipMerge = false);
std::unique_ptr<::mlir::Pass>
createPackStateLanesPass(unsigned maxWidth = 192);
/// Phase runners shared with the unified StateOptimizePass. Defined next to
/// the passes whose cores they drive.
void runDelayChainMergeRound(::mlir::func::FuncOp function, bool cascadeRound,
                             bool accumulateStats);
void runDelayChainFormAndShare(::mlir::func::FuncOp function,
                               bool accumulateStats);
void runRetimePipelines(::mlir::func::FuncOp function, unsigned maxStages,
                        unsigned maxExtraCombOps, unsigned maxCombDepth,
                        bool accumulateStats);
void runPackStateLanes(::mlir::func::FuncOp function, unsigned maxWidth);
void runEliminateDeadState(::mlir::func::FuncOp function);
/// Unified C++-only structural state optimization pipeline. Replaces the
/// explicit merge -> canonicalize/CSE -> cascade-merge -> retime ->
/// dead-state -> form/share-chains -> pack sequence in pycc with one
/// func-level pass that runs the same phases in order.
struct StateOptimizeTunables {
  unsigned retimeMaxStages = 0;
  unsigned retimeMaxExtraCombOps = 32;
  unsigned retimeMaxCombDepth = 32;
  unsigned packMaxWidth = 192; // 0 disables packing
};
std::unique_ptr<::mlir::Pass>
createStateOptimizePass(const StateOptimizeTunables &tunables = {});
std::unique_ptr<::mlir::Pass> createPackI1RegsPass();
/// After C++ state opt remaps identities, drop pyc.name values that are not
/// in the runtime observation demand set (@probe source, trace fields, or
/// debug_keep/observable/probe*/trace*).
std::unique_ptr<::mlir::Pass>
createApplyObservationDemandPass(std::string probePlanPath = {},
                                 std::string traceCodegenPlanPath = {});
std::unique_ptr<::mlir::Pass> createLowerSCFToPYCStaticPass();
std::unique_ptr<::mlir::Pass> createCheckFrontendContractPass();
std::unique_ptr<::mlir::Pass> createCheckHierarchyDisciplinePass();
std::unique_ptr<::mlir::Pass> createCheckNoDynamicPass();
std::unique_ptr<::mlir::Pass> createCheckCombCyclesPass();
std::unique_ptr<::mlir::Pass> createCheckClockDomainsPass();
std::unique_ptr<::mlir::Pass> createChangeDrivenSchedulePass();
std::unique_ptr<::mlir::Pass> createCheckFlatTypesPass();
std::unique_ptr<::mlir::Pass> createPrunePortsPass();
std::unique_ptr<::mlir::Pass> createEliminateDeadStatePass();
std::unique_ptr<::mlir::Pass> createEliminateDeadInstancesPass();
std::unique_ptr<::mlir::Pass> createSLPPackWiresPass();
std::unique_ptr<::mlir::Pass> createCheckLogicDepthPass(unsigned logicDepth);
std::unique_ptr<::mlir::Pass> createCollectCompileStatsPass();
std::unique_ptr<::mlir::Pass> createFlattenInstancesPass();
/// C++ emit prep: sets module comb chunk size and runs member placement.
/// Named values stay on the struct so probes and VCD sampling keep stable
/// addresses. A trace codegen plan, when given, must name fields that exist.
std::unique_ptr<::mlir::Pass>
createCppPlacementPass(unsigned combChunkNodes,
                       std::string traceCodegenPlanPath = {});
std::unique_ptr<::mlir::Pass> createVectorUnrollPass();

} // namespace pyc
