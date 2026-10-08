#include "pyc/Simulation/SimulationAST.h"

#include "pyc/Dialect/PYC/PYCOps.h"
#include "pyc/Dialect/PYC/PYCTypes.h"

#include "mlir/Dialect/Arith/IR/Arith.h"
#include "mlir/IR/Block.h"
#include "mlir/IR/BuiltinOps.h"
#include "mlir/IR/Region.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/STLExtras.h"

using namespace mlir;

namespace pyc {

bool isSimStateBoundary(Operation *op) {
  return isa<pyc::RegOp, pyc::FifoOp, pyc::ByteMemOp, pyc::SyncMemOp,
             pyc::SyncMemDPOp, pyc::AsyncFifoOp, pyc::CdcSyncOp>(op);
}

namespace {

enum class SeqStateKind : unsigned char { Visiting, NoSequential, HasSequential };

static bool hasSequentialState(func::FuncOp function, ModuleOp module,
                               llvm::DenseMap<Operation *, SeqStateKind> &memo) {
  if (!function || !module)
    return true;
  if (auto it = memo.find(function.getOperation()); it != memo.end())
    return it->second != SeqStateKind::NoSequential;
  memo[function.getOperation()] = SeqStateKind::Visiting;
  Region *region = function.getCallableRegion();
  if (!region || region->empty()) {
    memo[function.getOperation()] = SeqStateKind::HasSequential;
    return true;
  }
  bool stateful = false;
  for (Operation &op : region->front()) {
    if (isSimStateBoundary(&op)) {
      stateful = true;
      break;
    }
    if (auto inst = dyn_cast<pyc::InstanceOp>(op)) {
      auto callee = inst->getAttrOfType<FlatSymbolRefAttr>("callee");
      if (!callee ||
          hasSequentialState(
              module.lookupSymbol<func::FuncOp>(callee.getValue()), module,
              memo)) {
        stateful = true;
        break;
      }
    }
  }
  memo[function.getOperation()] =
      stateful ? SeqStateKind::HasSequential : SeqStateKind::NoSequential;
  return stateful;
}

static std::string capturePortPath(func::FuncOp function, unsigned index,
                                   bool result) {
  auto names = function->getAttrOfType<ArrayAttr>(result ? "result_names"
                                                     : "arg_names");
  if (names && index < names.size())
    if (auto name = dyn_cast<StringAttr>(names[index]))
      return name.getValue().str();
  return std::string(result ? "out" : "arg") + std::to_string(index);
}

static bool captureStructuralEmission(func::FuncOp function) {
  auto attr = function->getAttrOfType<StringAttr>("pyc.emit.structural");
  if (!attr)
    return false;
  llvm::StringRef value = attr.getValue();
  return value.equals_insensitive("true") || value == "1";
}

static SimulationASTValue captureValueFacts(Value value) {
  SimulationASTValue captured;
  captured.source = value;
  Type type = value.getType();
  if (auto vector = dyn_cast<VectorType>(type)) {
    captured.shape.append(vector.getShape().begin(), vector.getShape().end());
    type = vector.getElementType();
  }
  if (auto integer = dyn_cast<IntegerType>(type))
    captured.width = integer.getWidth();
  else if (isa<pyc::ClockType, pyc::ResetType>(type))
    captured.width = 1;
  for (OpOperand &use : value.getUses())
    (void)use, ++captured.useCount;
  if (Operation *def = value.getDefiningOp()) {
    captured.observable =
        def->hasAttr("pyc.name") || def->hasAttr("pyc.debug_keep");
    if (auto name = def->getAttrOfType<StringAttr>("pyc.name")) {
      captured.sourceNameBase = name.getValue().str();
      captured.sourceNameIsExplicit = true;
      if (def->getNumResults() == 1)
        captured.probeName = name.getValue().str();
    } else {
      captured.sourceNameBase = def->getName().getStringRef().str();
    }
  }
  return captured;
}

static void collectValueFacts(const SimulationASTNode &node,
                              SimulationAST &ast) {
  auto add = [&](Value value) {
    if (!ast.valueFacts.count(value))
      ast.valueFacts.try_emplace(value, captureValueFacts(value));
  };
  for (Value value : node.inputs)
    add(value);
  for (const SimulationASTRegion &region : node.regions)
    for (const SimulationASTBlock &block : region.blocks)
      for (Value argument : block.arguments)
        add(argument);
  for (const SimulationASTNode &child : node.children)
    collectValueFacts(child, ast);
  for (Value value : node.outputs) {
    add(value);
    ast.declarationValues.push_back(value);
  }
}

static std::optional<SimulationASTExpression> captureExpression(Operation &op) {
  std::optional<SimExprKind> kind;
  if (auto constant = dyn_cast<arith::ConstantOp>(&op)) {
    if (isa<IntegerType>(constant.getType()) &&
        isa<IntegerAttr>(constant.getValue()))
      kind = SimExprKind::Constant;
  }
#define AST_KIND(Op, Kind) if (isa<pyc::Op>(&op)) kind = SimExprKind::Kind
  AST_KIND(ConstantOp, Constant);
  AST_KIND(AliasOp, Alias);
  AST_KIND(ResetActiveOp, ResetActive);
  AST_KIND(AddOp, Add);
  AST_KIND(SubOp, Sub);
  AST_KIND(MulOp, Mul);
  AST_KIND(UdivOp, Udiv);
  AST_KIND(UremOp, Urem);
  AST_KIND(SdivOp, Sdiv);
  AST_KIND(SremOp, Srem);
  AST_KIND(MuxOp, Mux);
  if (isa<arith::SelectOp>(&op)) kind = SimExprKind::Select;
  AST_KIND(AndOp, And);
  AST_KIND(OrOp, Or);
  AST_KIND(XorOp, Xor);
  AST_KIND(NotOp, Not);
  AST_KIND(ConcatOp, Concat);
  AST_KIND(EqOp, Eq);
  AST_KIND(UltOp, Ult);
  AST_KIND(SltOp, Slt);
  AST_KIND(TruncOp, Trunc);
  AST_KIND(ZextOp, Zext);
  AST_KIND(SextOp, Sext);
  AST_KIND(ExtractOp, Extract);
  AST_KIND(ShliOp, Shli);
  AST_KIND(LshriOp, Lshri);
  AST_KIND(AshriOp, Ashri);
  AST_KIND(ShlOp, Shl);
  AST_KIND(LshrOp, Lshr);
  AST_KIND(AshrOp, Ashr);
  AST_KIND(VGetOp, VGet);
  AST_KIND(VCreateOp, VCreate);
  AST_KIND(VBroadcastOp, VBroadcast);
  AST_KIND(VBroadcastDimOp, VBroadcastDim);
  AST_KIND(VOrReduceOp, VOrReduce);
  AST_KIND(VAndReduceOp, VAndReduce);
  AST_KIND(VAddReduceOp, VAddReduce);
#undef AST_KIND
  if (!kind)
    return std::nullopt;
  SimulationASTExpression expr;
  expr.kind = *kind;
  if (auto constant = dyn_cast<pyc::ConstantOp>(&op))
    expr.constant = constant.getValueAttr().getValue();
  else if (auto constant = dyn_cast<arith::ConstantOp>(&op))
    expr.constant = cast<IntegerAttr>(constant.getValue()).getValue();
  if (auto extract = dyn_cast<pyc::ExtractOp>(&op))
    expr.immediate = extract.getLsb();
  else if (auto shift = dyn_cast<pyc::ShliOp>(&op))
    expr.immediate = shift.getAmount();
  else if (auto shift = dyn_cast<pyc::LshriOp>(&op))
    expr.immediate = shift.getAmount();
  else if (auto shift = dyn_cast<pyc::AshriOp>(&op))
    expr.immediate = shift.getAmount();
  else if (auto get = dyn_cast<pyc::VGetOp>(&op))
    expr.immediate = get.getIndex();
  else if (auto broadcast = dyn_cast<pyc::VBroadcastOp>(&op))
    expr.immediate = broadcast.getSize();
  else if (auto broadcast = dyn_cast<pyc::VBroadcastDimOp>(&op)) {
    expr.immediate = broadcast.getSize();
    expr.dimension = broadcast.getDim();
  }
  if (isa<pyc::VOrReduceOp, pyc::VAndReduceOp, pyc::VAddReduceOp>(&op)) {
    if (auto dimension = op.getAttrOfType<IntegerAttr>("dim"))
      expr.dimension = dimension.getInt();
    if (auto mode = op.getAttrOfType<StringAttr>("mode"))
      expr.treeReduce = mode.getValue() == "tree";
  }
  return expr;
}

static void captureNodeFacts(
    Operation &op, SimulationASTNode &node, ModuleOp module,
    llvm::DenseMap<Operation *, SeqStateKind> &sequentialMemo) {
  if (node.expression)
    node.kind = SimNodeKind::Expression;
  else if (isa<pyc::CombOp>(&op)) node.kind = SimNodeKind::CombRegion;
  else if (isa<pyc::YieldOp>(&op)) node.kind = SimNodeKind::Yield;
  else if (isa<pyc::WireOp>(&op)) node.kind = SimNodeKind::Wire;
  else if (isa<pyc::AssignOp>(&op)) node.kind = SimNodeKind::Assign;
  else if (isa<pyc::AssertOp>(&op)) node.kind = SimNodeKind::Assert;
  else if (isa<func::ReturnOp>(&op)) node.kind = SimNodeKind::Return;
  else if (isa<pyc::RegOp>(&op)) node.kind = SimNodeKind::Reg;
  else if (isa<pyc::FifoOp>(&op)) node.kind = SimNodeKind::Fifo;
  else if (isa<pyc::ByteMemOp>(&op)) node.kind = SimNodeKind::ByteMem;
  else if (isa<pyc::SyncMemOp>(&op)) node.kind = SimNodeKind::SyncMem;
  else if (isa<pyc::SyncMemDPOp>(&op)) node.kind = SimNodeKind::SyncMemDP;
  else if (isa<pyc::AsyncFifoOp>(&op)) node.kind = SimNodeKind::AsyncFifo;
  else if (isa<pyc::CdcSyncOp>(&op)) node.kind = SimNodeKind::CdcSync;
  else if (isa<pyc::InstanceOp>(&op)) node.kind = SimNodeKind::Instance;

  if (node.kind == SimNodeKind::CdcSync) {
    node.cdcStages = 2;
    if (auto stages = op.getAttrOfType<IntegerAttr>("stages"))
      node.cdcStages = stages.getValue().getZExtValue();
  }
  if (node.kind == SimNodeKind::Fifo ||
      node.kind == SimNodeKind::AsyncFifo ||
      node.kind == SimNodeKind::ByteMem ||
      node.kind == SimNodeKind::SyncMem ||
      node.kind == SimNodeKind::SyncMemDP) {
    if (auto depth = op.getAttrOfType<IntegerAttr>("depth"))
      node.primitiveDepth = depth.getValue().getZExtValue();
  }
  if (node.kind == SimNodeKind::ByteMem ||
      node.kind == SimNodeKind::SyncMem ||
      node.kind == SimNodeKind::SyncMemDP)
    if (auto name = op.getAttrOfType<StringAttr>("name")) {
      node.primitiveName = name.getValue().str();
      node.primitiveHasName = true;
    }
  if (node.kind == SimNodeKind::Instance) {
    if (auto callee = op.getAttrOfType<FlatSymbolRefAttr>("callee"))
      node.instanceCallee = callee.getValue().str();
    if (auto name = op.getAttrOfType<StringAttr>("name")) {
      node.instanceName = name.getValue().str();
      node.instanceHasName = true;
    }
    if (auto shortName = op.getAttrOfType<StringAttr>("short_name")) {
      node.instanceShortName = shortName.getValue().str();
      node.instanceHasShortName = true;
    }
    func::FuncOp callee;
    if (module && !node.instanceCallee.empty())
      callee = module.lookupSymbol<func::FuncOp>(node.instanceCallee);
    if (callee) {
      node.instanceCalleeResolved = true;
      for (unsigned i = 0; i < callee.getNumArguments(); ++i)
        node.instanceInputPortPaths.push_back(
            capturePortPath(callee, i, false));
      for (unsigned i = 0; i < callee.getNumResults(); ++i)
        node.instanceOutputPortPaths.push_back(
            capturePortPath(callee, i, true));
    }
    node.stateBoundary = hasSequentialState(callee, module, sequentialMemo);
  }
  if (isSimStateBoundary(&op))
    node.stateBoundary = true;
  if (node.kind == SimNodeKind::Assert) {
    if (auto message = op.getAttrOfType<StringAttr>("msg"))
      node.assertionMessage = message.getValue().str();
    else
      node.assertionMessage = "pyc.assert failed";
  }
}

static SimulationASTNode capture(
    Operation &op, ModuleOp module,
    llvm::DenseMap<Operation *, SeqStateKind> &sequentialMemo) {
  SimulationASTNode node;
  node.source = &op;
  node.opcode = op.getName().getStringRef().str();
  node.expression = captureExpression(op);
  captureNodeFacts(op, node, module, sequentialMemo);
  node.inputs.append(op.getOperands().begin(), op.getOperands().end());
  node.outputs.append(op.getResults().begin(), op.getResults().end());
  for (Region &region : op.getRegions()) {
    SimulationASTRegion astRegion;
    for (Block &block : region) {
      SimulationASTBlock astBlock;
      astBlock.arguments.append(block.getArguments().begin(), block.getArguments().end());
      astBlock.firstChild = static_cast<unsigned>(node.children.size());
      for (Operation &child : block)
        node.children.push_back(capture(child, module, sequentialMemo));
      astBlock.childCount = static_cast<unsigned>(node.children.size()) - astBlock.firstChild;
      astRegion.blocks.push_back(std::move(astBlock));
    }
    node.regions.push_back(std::move(astRegion));
  }
  return node;
}

static LogicalResult verifyNode(
    const SimulationASTNode &node, ModuleOp module,
    llvm::DenseMap<Operation *, SeqStateKind> &sequentialMemo) {
  if (!node.source || node.opcode != node.source->getName().getStringRef() ||
      node.inputs.size() != node.source->getNumOperands() ||
      node.outputs.size() != node.source->getNumResults())
    return failure();
  auto expectedExpr = captureExpression(*node.source);
  if (node.expression.has_value() != expectedExpr.has_value())
    return failure();
  if (expectedExpr &&
      (node.expression->kind != expectedExpr->kind ||
       node.expression->constant != expectedExpr->constant ||
       node.expression->immediate != expectedExpr->immediate ||
       node.expression->dimension != expectedExpr->dimension ||
       node.expression->treeReduce != expectedExpr->treeReduce))
    return failure();
  SimulationASTNode expected;
  expected.expression = std::move(expectedExpr);
  captureNodeFacts(*node.source, expected, module, sequentialMemo);
  if (node.kind != expected.kind ||
      node.cdcStages != expected.cdcStages ||
      node.primitiveDepth != expected.primitiveDepth ||
      node.primitiveName != expected.primitiveName ||
      node.primitiveHasName != expected.primitiveHasName ||
      node.instanceCallee != expected.instanceCallee ||
      node.instanceInputPortPaths != expected.instanceInputPortPaths ||
      node.instanceOutputPortPaths != expected.instanceOutputPortPaths ||
      node.instanceCalleeResolved != expected.instanceCalleeResolved ||
      node.instanceName != expected.instanceName ||
      node.instanceShortName != expected.instanceShortName ||
      node.instanceHasName != expected.instanceHasName ||
      node.instanceHasShortName != expected.instanceHasShortName ||
      node.assertionMessage != expected.assertionMessage ||
      node.stateBoundary != expected.stateBoundary)
    return failure();
  for (auto [i, value] : llvm::enumerate(node.inputs))
    if (value != node.source->getOperand(i))
      return failure();
  for (auto [i, value] : llvm::enumerate(node.outputs))
    if (value != node.source->getResult(i))
      return failure();
  if (node.regions.size() != node.source->getNumRegions())
    return failure();
  size_t childIndex = 0;
  for (auto [regionIndex, region] : llvm::enumerate(node.source->getRegions())) {
    const SimulationASTRegion &astRegion = node.regions[regionIndex];
    if (astRegion.blocks.size() != region.getBlocks().size())
      return failure();
    for (auto [blockIndex, block] : llvm::enumerate(region.getBlocks())) {
      const SimulationASTBlock &astBlock = astRegion.blocks[blockIndex];
      if (astBlock.firstChild != childIndex ||
          astBlock.childCount != block.getOperations().size() ||
          astBlock.arguments.size() != block.getNumArguments())
        return failure();
      for (auto [argIndex, value] : llvm::enumerate(astBlock.arguments))
        if (value != block.getArgument(argIndex))
          return failure();
      for (Operation &child : block) {
        if (childIndex >= node.children.size() ||
            node.children[childIndex].source != &child ||
            failed(verifyNode(node.children[childIndex], module,
                              sequentialMemo)))
          return failure();
        ++childIndex;
      }
    }
  }
  return childIndex == node.children.size() ? success() : failure();
}

} // namespace

FailureOr<SimulationAST> buildSimulationAST(func::FuncOp function) {
  if (!function || function.isDeclaration())
    return failure();
  if (!llvm::hasSingleElement(function.getBody())) {
    function.emitError("simulation AST requires a single-block function");
    return failure();
  }
  SimulationAST ast;
  ast.function = function;
  ast.functionName = function.getSymName().str();
  ast.structuralEmission = captureStructuralEmission(function);
  for (unsigned i = 0; i < function.getNumArguments(); ++i)
    ast.inputPortPaths.push_back(capturePortPath(function, i, false));
  for (unsigned i = 0; i < function.getNumResults(); ++i)
    ast.outputPortPaths.push_back(capturePortPath(function, i, true));
  ModuleOp module = function->getParentOfType<ModuleOp>();
  llvm::DenseMap<Operation *, SeqStateKind> sequentialMemo;
  for (Operation &op : function.getBody().front())
    ast.body.push_back(capture(op, module, sequentialMemo));
  for (Value argument : function.getArguments())
    ast.valueFacts.try_emplace(argument, captureValueFacts(argument));
  for (const SimulationASTNode &node : ast.body)
    collectValueFacts(node, ast);
  if (failed(ast.verify()))
    return failure();
  return ast;
}

LogicalResult SimulationAST::verify() const {
  func::FuncOp f = function;
  if (!f || !llvm::hasSingleElement(f.getBody()))
    return failure();
  if (functionName != f.getSymName().str() ||
      structuralEmission != captureStructuralEmission(f) ||
      inputPortPaths.size() != f.getNumArguments() ||
      outputPortPaths.size() != f.getNumResults())
    return f.emitError("simulation AST function metadata is stale");
  for (unsigned i = 0; i < inputPortPaths.size(); ++i)
    if (inputPortPaths[i] != capturePortPath(f, i, false))
      return f.emitError("simulation AST input port is stale");
  for (unsigned i = 0; i < outputPortPaths.size(); ++i)
    if (outputPortPaths[i] != capturePortPath(f, i, true))
      return f.emitError("simulation AST output port is stale");
  llvm::DenseSet<Value> seenValues;
  for (Value argument : f.getArguments())
    seenValues.insert(argument);
  llvm::SmallVector<Value> sourceDeclarations;
  f.walk([&](Operation *op) {
    for (Value operand : op->getOperands())
      seenValues.insert(operand);
    for (Value result : op->getResults()) {
      seenValues.insert(result);
      sourceDeclarations.push_back(result);
    }
    for (Region &region : op->getRegions())
      for (Block &block : region)
        for (Value argument : block.getArguments())
          seenValues.insert(argument);
  });
  if (sourceDeclarations != declarationValues ||
      seenValues.size() != valueFacts.size())
    return f.emitError("simulation AST value inventory is stale");
  for (Value value : seenValues) {
    auto it = valueFacts.find(value);
    if (it == valueFacts.end())
      return f.emitError("simulation AST omitted a value");
    SimulationASTValue expected = captureValueFacts(value);
    const SimulationASTValue &actual = it->second;
    if (actual.source != value || actual.width != expected.width ||
        actual.shape != expected.shape ||
        actual.useCount != expected.useCount ||
        actual.observable != expected.observable ||
        actual.sourceNameBase != expected.sourceNameBase ||
        actual.sourceNameIsExplicit != expected.sourceNameIsExplicit ||
        actual.probeName != expected.probeName)
      return f.emitError("simulation AST value metadata is stale");
  }
  ModuleOp module = f->getParentOfType<ModuleOp>();
  llvm::DenseMap<Operation *, SeqStateKind> sequentialMemo;
  size_t index = 0;
  for (Operation &op : f.getBody().front()) {
    if (index >= body.size() || body[index].source != &op ||
        failed(verifyNode(body[index], module, sequentialMemo)))
      return f.emitError("simulation AST does not match legalized PYC IR");
    ++index;
  }
  if (index != body.size())
    return f.emitError("simulation AST contains an extra operation");
  return success();
}

} // namespace pyc
