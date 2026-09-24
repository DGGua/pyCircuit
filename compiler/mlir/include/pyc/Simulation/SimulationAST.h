#pragma once

#include "mlir/Dialect/Func/IR/FuncOps.h"
#include "mlir/Support/LogicalResult.h"
#include "llvm/ADT/APInt.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallVector.h"

#include <optional>
#include <cstdint>
#include <string>
#include <vector>

namespace pyc {

enum class SimExprKind : unsigned char {
  Constant, Alias, ResetActive, Add, Sub, Mul, Udiv, Urem, Sdiv,
  Srem, Mux, Select, And, Or, Xor, Not, Concat, Eq, Ult, Slt,
  Trunc, Zext, Sext, Extract, Shli, Lshri, Ashri, Shl, Lshr, Ashr,
  VGet, VCreate, VBroadcast, VBroadcastDim, VOrReduce, VAndReduce,
  VAddReduce
};

enum class SimNodeKind : unsigned char {
  Unknown, Expression, PureGroup, CombRegion, Yield, Wire, Assign, Assert,
  Return, Reg, Fifo, ByteMem, SyncMem, SyncMemDP, AsyncFifo, CdcSync,
  Instance
};

// The AST freezes pure-operation semantics before graph construction. Source
// handles below remain available for structural verification and diagnostics.
struct SimulationASTExpression {
  SimExprKind kind = SimExprKind::Alias;
  llvm::APInt constant = llvm::APInt(1, 0);
  int64_t immediate = 0;
  int64_t dimension = -1;
  bool treeReduce = false;
};

// A hierarchical snapshot of the legalized PYC operation tree. Values and
// source operations are borrowed from MLIR and remain valid while it is
// unchanged. The AST preserves regions before graph nodes are formed.
struct SimulationASTBlock {
  llvm::SmallVector<mlir::Value> arguments;
  unsigned firstChild = 0;
  unsigned childCount = 0;
};

struct SimulationASTRegion {
  std::vector<SimulationASTBlock> blocks;
};

struct SimulationASTNode {
  mlir::Operation *source = nullptr;
  std::string opcode;
  SimNodeKind kind = SimNodeKind::Unknown;
  std::optional<SimulationASTExpression> expression;
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
  std::string assertionMessage;
  bool stateBoundary = false;
  llvm::SmallVector<mlir::Value> inputs;
  llvm::SmallVector<mlir::Value> outputs;
  std::vector<SimulationASTRegion> regions;
  std::vector<SimulationASTNode> children;
};

struct SimulationASTValue {
  mlir::Value source;
  unsigned width = 0;
  llvm::SmallVector<int64_t, 2> shape;
  unsigned useCount = 0;
  bool observable = false;
  std::string sourceNameBase;
  bool sourceNameIsExplicit = false;
  std::string probeName;
};

struct SimulationAST {
  mlir::func::FuncOp function;
  std::string functionName;
  llvm::SmallVector<std::string> inputPortPaths;
  llvm::SmallVector<std::string> outputPortPaths;
  bool structuralEmission = false;
  std::vector<SimulationASTNode> body;
  llvm::DenseMap<mlir::Value, SimulationASTValue> valueFacts;
  llvm::SmallVector<mlir::Value> declarationValues;

  mlir::LogicalResult verify() const;
};

mlir::FailureOr<SimulationAST> buildSimulationAST(mlir::func::FuncOp function);

bool isSimStateBoundary(mlir::Operation *op);

} // namespace pyc
