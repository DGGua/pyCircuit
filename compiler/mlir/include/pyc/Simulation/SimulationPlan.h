#pragma once

#include "pyc/Simulation/SimGraph.h"

#include "mlir/Dialect/Func/IR/FuncOps.h"
#include "mlir/IR/BuiltinOps.h"
#include "mlir/Support/LogicalResult.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/SmallVector.h"

#include <vector>

namespace pyc {

struct InstanceCachePlan {
  unsigned packedWords = 0;
  bool usePackedWords = false;
  bool invalidateOnCommit = true;
};

struct InstanceInterfacePlan {
  std::vector<std::string> inputPorts;
  std::vector<std::string> outputPorts;
};

struct ScheduledComponent {
  llvm::SmallVector<unsigned> nodeIds;
  bool cyclic = false;
  unsigned iterationLimit = 0;
};

struct GroupActivationPlan {
  llvm::SmallVector<unsigned> inputIds;
  llvm::SmallVector<llvm::APInt> usedBits;
  llvm::SmallVector<llvm::APInt> vectorLanes;
  llvm::SmallVector<llvm::APInt> vectorElements;
};

struct GroupPropagationTarget {
  unsigned groupNodeId = 0;
  llvm::SmallVector<unsigned> valueIds;
  llvm::SmallVector<llvm::APInt> usedBits;
  llvm::SmallVector<llvm::APInt> vectorLanes;
  llvm::SmallVector<llvm::APInt> vectorElements;
};

enum class SimStatementKind : unsigned char { Expression, MuxCondition };

struct PlannedMuxAssignment {
  unsigned resultId = 0;
  unsigned trueValueId = 0;
  unsigned falseValueId = 0;
};

struct SimStatement {
  SimStatementKind kind = SimStatementKind::Expression;
  unsigned expressionId = ~0u;
  unsigned selectorId = ~0u;
  llvm::SmallVector<PlannedMuxAssignment> muxAssignments;
};

struct GroupStatementPlan {
  llvm::SmallVector<unsigned> replicatedExpressionIds;
  llvm::SmallVector<llvm::SmallVector<SimStatement>> chunks;
};

struct CombRegionStatementPlan {
  llvm::SmallVector<llvm::SmallVector<SimCombStep>> chunks;
};

enum class SimNodeActionKind : unsigned char {
  Skip, Group, Assign, Assert, CombRegion, Expression,
  Fifo, AsyncFifo, ByteMem, Instance
};

struct SimNodeAction {
  SimNodeActionKind kind = SimNodeActionKind::Skip;
  unsigned targetId = ~0u;
  unsigned sourceId = ~0u;
  unsigned conditionId = ~0u;
  std::string message;
  unsigned regionId = ~0u;
  unsigned expressionId = ~0u;
};

struct ResetGroupPlan {
  unsigned clockValueId = 0;
  unsigned resetValueId = 0;
  llvm::SmallVector<unsigned> regNodeIds;
};

enum class SimTickActionKind : unsigned char {
  ResetGroup, Reg, Fifo, ByteMem, SyncMem, SyncMemDP, AsyncFifo, CdcSync
};

struct SimTickAction {
  SimTickActionKind kind;
  // A reset-group index for ResetGroup, otherwise a graph node ID.
  unsigned id = 0;
};

struct OperationOrder {
  llvm::SmallVector<unsigned> regs;
  llvm::SmallVector<unsigned> fifos;
  llvm::SmallVector<unsigned> byteMems;
  llvm::SmallVector<unsigned> syncMems;
  llvm::SmallVector<unsigned> syncMemDPs;
  llvm::SmallVector<unsigned> asyncFifos;
  llvm::SmallVector<unsigned> cdcSyncs;
  llvm::SmallVector<unsigned> instances;
  llvm::SmallVector<unsigned> combs;
};

enum class SimulationPhase { Comb, TickCompute, TickCommit };

struct SimulationPlan {
  SimGraph graph;
  OperationOrder operationOrder;
  llvm::SmallVector<llvm::SmallVector<unsigned>> instanceTickChunks;
  llvm::SmallVector<llvm::SmallVector<unsigned>> primitiveEvalChunks;
  llvm::SmallVector<SimulationPhase> stepPhases;
  llvm::SmallVector<unsigned> combNodeOrder;
  llvm::SmallVector<unsigned> evalNodeOrder;
  bool combTopological = false;
  bool evalTopological = false;
  llvm::SmallVector<ScheduledComponent> sccOrder;
  bool useSccWorklist = false;
  llvm::DenseMap<unsigned, InstanceCachePlan> instanceCaches;
  llvm::DenseMap<unsigned, InstanceInterfacePlan> instanceInterfaces;
  llvm::DenseSet<unsigned> fingerprintInputs;
  llvm::DenseSet<unsigned> invalidateCachesOnCommit;
  // Graph node IDs and value IDs are the planning vocabulary. Source handles
  // are resolved only when lowering effectful operations in the emitter.
  llvm::DenseMap<unsigned, unsigned> groupByNode;
  llvm::DenseMap<unsigned, GroupActivationPlan> groupActivations;
  // Graph optimization is lowered to executable statement sequences before
  // the emitter renders C++ syntax.
  llvm::DenseMap<unsigned, GroupStatementPlan> groupStatements;
  llvm::DenseMap<unsigned, CombRegionStatementPlan> combRegionStatements;
  // One executable action per graph node. The emitter renders this action
  // without choosing behavior from an MLIR operation or graph node kind.
  llvm::SmallVector<SimNodeAction> nodeActions;
  // A packed activity bit replaces input comparison only when every input of
  // the target group has a unique, scheduled pure-group producer.
  llvm::DenseSet<unsigned> packedActivationGroups;
  llvm::DenseMap<unsigned, llvm::SmallVector<GroupPropagationTarget, 0>>
      groupPropagations;
  llvm::SmallVector<ResetGroupPlan> resetGroups;
  llvm::SmallVector<SimTickAction> localTickComputeActions;
  llvm::SmallVector<SimTickAction> localTickCommitActions;
  // Observable graph values that are aliases or comb passthroughs of a local
  // register output retain register probe semantics.
  llvm::DenseMap<unsigned, unsigned> registerProbeTargets;
  unsigned primitiveCount = 0;
  unsigned fallbackIterationLimit = 0;
  unsigned primitiveGroupSize = 64;
  unsigned evalChunkNodes = 256;
  unsigned combChunkNodes = 256;
  unsigned sccChunkNodes = 256;
  unsigned tickChunkNodes = 256;

  mlir::LogicalResult verify() const;
};

struct ModuleSimulationPlan {
  mlir::ModuleOp module;
  std::vector<SimulationPlan> functions; // dependency order
};

struct SimulationPlanningOptions {
  unsigned evalChunkNodes = 256;
  unsigned combChunkNodes = 256;
};

mlir::FailureOr<SimulationPlan> buildSimulationPlan(SimGraph graph,
                                                    const SimulationPlanningOptions &options = {});
mlir::FailureOr<ModuleSimulationPlan> buildModuleSimulationPlan(
    mlir::ModuleOp module, std::vector<SimulationPlan> functions);

} // namespace pyc
