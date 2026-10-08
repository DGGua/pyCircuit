#pragma once

#include "pyc/Simulation/SimulationAST.h"

#include "mlir/Dialect/Func/IR/FuncOps.h"
#include "mlir/Support/LogicalResult.h"
#include "llvm/ADT/APInt.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallVector.h"

#include <cstdint>
#include <string>

namespace pyc {

// The execution DAG owns these types and operation descriptions. An optional
// MLIR source handle identifies the original signal for diagnostics and
// externally visible names; expression semantics live in SimExpr below.
struct SimType {
  unsigned width = 0;
  llvm::SmallVector<int64_t, 2> shape;
};

struct SimValue {
  SimType type;
  mlir::Value source;
  unsigned sourceUseCount = 0;
  bool observable = false;
  llvm::SmallVector<unsigned, 2> expressionUsers;
  std::string probeName;
  std::string sourceNameBase;
  bool sourceNameIsExplicit = false;
  bool sourceBacked = false;
};

struct SimExpr {
  SimExprKind kind;
  unsigned result = 0;
  llvm::SmallVector<unsigned, 3> operands;
  llvm::APInt constant = llvm::APInt(1, 0);
  int64_t immediate = 0;
  int64_t dimension = -1;
  bool treeReduce = false;
  // Stable order captured at graph construction. Passes use this when
  // coarsening nodes from different source runs.
  unsigned sourceOrder = 0;
  // A cheap, single-use expression can be emitted in its consumer's RHS.
  bool inlineIntoConsumer = false;
  // A source expression fully replicated into consumer groups no longer needs
  // a standalone evaluation.
  bool omitOriginalEvaluation = false;
  // Source is for location/name lookup only; graph passes can create an
  // expression without one and must not inspect it for semantics.
  mlir::Operation *source = nullptr;
};

struct MuxConditionBatch {
  unsigned begin = 0;
  unsigned end = 0;
  unsigned selectorId = 0;
};

// A simulation view of legalized PYC IR. Operations and values are borrowed
// from the function and remain valid only while that IR is unchanged.
struct SimNode {
  SimNodeKind kind = SimNodeKind::Unknown;
  std::string sourceOperationName;
  mlir::Operation *op = nullptr;
  llvm::SmallVector<mlir::Operation *> operations;
  llvm::SmallVector<unsigned> expressionIds;
  llvm::SmallVector<unsigned> replicatedExpressionIds;
  llvm::SmallVector<mlir::Value> inputs;
  llvm::SmallVector<mlir::Value> outputs;
  llvm::SmallVector<unsigned> inputIds;
  llvm::SmallVector<unsigned> outputIds;
  mlir::Value assignedValue;
  mlir::Value assignedFrom;
  unsigned assignedValueId = ~0u;
  unsigned assignedFromId = ~0u;
  unsigned assertionConditionId = ~0u;
  std::string assertionMessage;
  uint64_t cdcStages = 0;
  uint64_t primitiveDepth = 0;
  std::string primitiveName;
  bool primitiveHasName = false;
  std::string instanceCallee;
  llvm::SmallVector<std::string> instanceInputPortPaths;
  llvm::SmallVector<std::string> instanceOutputPortPaths;
  bool instanceCalleeResolved = false;
  std::string instanceName;
  std::string instanceShortName;
  bool instanceHasName = false;
  bool instanceHasShortName = false;
  unsigned combRegionId = ~0u;
  bool stateBoundary = false;
  bool observable = false;
  // Set by the graph activation pass for a pure combinational group.
  bool activateOnInputChange = false;
  // Exact demanded bits for integer group inputs. A one-bit all-ones mask is
  // the sentinel for non-integer inputs, which must be compared in full.
  llvm::SmallVector<llvm::APInt> activationUsedBits;
  // Outer vector lanes needed by this group. Scalar inputs use a one-bit
  // all-ones sentinel; an all-ones vector mask compares the whole input.
  llvm::SmallVector<llvm::APInt> activationVectorLanes;
  // Rank-2 inputs can additionally select individual elements in row-major
  // order. Other inputs use a one-bit all-ones sentinel.
  llvm::SmallVector<llvm::APInt> activationVectorElements;
  // Contiguous muxes sharing a scalar condition can use one C++ branch.
  llvm::SmallVector<MuxConditionBatch> muxConditionBatches;
};

struct SimEdge {
  unsigned producer = 0;
  unsigned consumer = 0;
  unsigned valueId = 0;
  mlir::Value value;
  bool crossesStateBoundary = false;
  bool reachesObservable = false;
};

struct SimCombStep {
  // A step is either a captured pure expression or a nested comb region.
  unsigned expressionId = ~0u;
  unsigned nestedRegionId = ~0u;
};

struct SimCombRegion {
  // Source is retained for verification and diagnostics, not codegen semantics.
  mlir::Operation *source = nullptr;
  llvm::SmallVector<unsigned> inputIds;
  llvm::SmallVector<unsigned> argumentIds;
  llvm::SmallVector<unsigned> resultIds;
  llvm::SmallVector<unsigned> yieldIds;
  llvm::SmallVector<SimCombStep> steps;
};

struct SimGraph {
  mlir::func::FuncOp function;
  std::string functionName;
  bool structuralEmission = false;
  llvm::SmallVector<SimValue, 0> values;
  llvm::SmallVector<SimExpr, 0> expressions;
  llvm::DenseMap<mlir::Value, unsigned> valueIds;
  llvm::DenseMap<mlir::Operation *, unsigned> expressionIds;
  llvm::SmallVector<SimCombRegion, 0> combRegions;
  // Backward bit demand from observable roots and non-expression operations.
  // Vectors and opaque values use a one-bit full-demand sentinel.
  llvm::SmallVector<llvm::APInt, 0> valueUsedBits;
  llvm::SmallVector<SimNode, 0> topNodes;
  // Includes nested operations, including the contents of existing pyc.comb.
  llvm::SmallVector<SimNode, 0> fineNodes;
  llvm::SmallVector<unsigned> inputValueIds;
  llvm::SmallVector<std::string> inputPortPaths;
  llvm::SmallVector<std::string> outputPortPaths;
  llvm::SmallVector<unsigned> outputValueIds;
  llvm::SmallVector<unsigned> namedProbeValueIds;
  // Source result declaration order is captured before graph optimization.
  llvm::SmallVector<unsigned> declarationValueIds;
  // Data dependencies between topNodes. State and observable boundaries are
  // explicit so combinational DAG passes can avoid crossing them.
  llvm::SmallVector<SimEdge> edges;
  bool hasAmbiguousDrivers = false;
  // Indices into topNodes whose operations are grouped for C++ evaluation.
  llvm::SmallVector<unsigned> groupedNodes;

  mlir::LogicalResult rebuildEdges();
  mlir::LogicalResult verify();
};

bool isFusableSimCombOp(mlir::Operation *op);
mlir::FailureOr<SimGraph> buildSimGraph(const SimulationAST &ast);

} // namespace pyc
