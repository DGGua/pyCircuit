#include "pyc/Simulation/SimGraph.h"

#include "pyc/Dialect/PYC/PYCOps.h"
#include "pyc/Dialect/PYC/PYCTypes.h"

#include "mlir/Dialect/Arith/IR/Arith.h"
#include "mlir/Dialect/Func/IR/FuncOps.h"
#include "mlir/IR/BuiltinOps.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/STLExtras.h"

#include <cassert>
#include <limits>

using namespace mlir;

namespace pyc {

bool isFusableSimCombOp(Operation *op) {
  if (auto constant = dyn_cast<arith::ConstantOp>(op))
    return isa<IntegerType>(constant.getType()) &&
           isa<IntegerAttr>(constant.getValue());
  return isa<pyc::ConstantOp, pyc::AddOp, pyc::SubOp, pyc::MulOp,
             pyc::UdivOp, pyc::UremOp, pyc::SdivOp, pyc::SremOp,
             pyc::MuxOp, pyc::AndOp, pyc::OrOp, pyc::XorOp, pyc::NotOp,
             pyc::ConcatOp, pyc::AliasOp, pyc::ResetActiveOp, pyc::EqOp,
             pyc::UltOp, pyc::SltOp, pyc::TruncOp, pyc::ZextOp,
             pyc::SextOp, pyc::ExtractOp, pyc::ShliOp, pyc::LshriOp,
             pyc::AshriOp, pyc::ShlOp, pyc::LshrOp, pyc::AshrOp,
             pyc::VGetOp, pyc::VCreateOp, pyc::VBroadcastOp,
             pyc::VBroadcastDimOp,
             pyc::VOrReduceOp, pyc::VAndReduceOp, pyc::VAddReduceOp,
             arith::SelectOp>(op);
}

namespace {

static unsigned getOrAddValue(SimGraph &graph, const SimulationAST &ast,
                              Value value) {
  if (auto it = graph.valueIds.find(value); it != graph.valueIds.end())
    return it->second;
  auto facts = ast.valueFacts.find(value);
  assert(facts != ast.valueFacts.end() && "simulation AST omitted a value");
  const SimulationASTValue &captured = facts->second;
  unsigned id = graph.values.size();
  graph.valueIds.try_emplace(value, id);
  SimType type;
  type.width = captured.width;
  type.shape = captured.shape;
  graph.values.push_back(SimValue{type, value, captured.useCount,
                                  captured.observable});
  graph.values.back().sourceBacked = true;
  graph.values.back().sourceNameBase = captured.sourceNameBase;
  graph.values.back().sourceNameIsExplicit = captured.sourceNameIsExplicit;
  graph.values.back().probeName = captured.probeName;
  return id;
}

static LogicalResult captureExpressions(
    SimGraph &graph, const SimulationAST &ast,
    const llvm::DenseMap<Operation *, const SimulationASTExpression *>
        &descriptions) {
  for (Value argument : graph.function.getArguments())
    (void)getOrAddValue(graph, ast, argument);
  for (SimNode &node : graph.fineNodes) {
    Operation *op = node.op;
    for (Value input : node.inputs)
      (void)getOrAddValue(graph, ast, input);
    for (Value output : node.outputs)
      (void)getOrAddValue(graph, ast, output);
    for (Value input : node.inputs)
      node.inputIds.push_back(getOrAddValue(graph, ast, input));
    for (Value output : node.outputs)
      node.outputIds.push_back(getOrAddValue(graph, ast, output));
    auto description = descriptions.find(op);
    if (description == descriptions.end()) {
      if (isFusableSimCombOp(op))
        return op->emitError("simulation AST omitted a pure expression");
      continue;
    }
    if (!isFusableSimCombOp(op) || node.outputs.size() != 1)
      return op->emitError("cannot capture simulation expression");
    const SimulationASTExpression &captured = *description->second;
    SimExpr expr;
    expr.kind = captured.kind;
    expr.source = op;
    expr.sourceOrder = graph.expressions.size();
    expr.result = getOrAddValue(graph, ast, node.outputs.front());
    for (Value input : node.inputs)
      expr.operands.push_back(getOrAddValue(graph, ast, input));
    expr.constant = captured.constant;
    expr.immediate = captured.immediate;
    expr.dimension = captured.dimension;
    expr.treeReduce = captured.treeReduce;
    unsigned id = graph.expressions.size();
    graph.expressions.push_back(std::move(expr));
    graph.expressionIds.try_emplace(op, id);
  }
  for (SimNode &node : graph.topNodes) {
    for (Value input : node.inputs)
      node.inputIds.push_back(getOrAddValue(graph, ast, input));
    for (Value output : node.outputs)
      node.outputIds.push_back(getOrAddValue(graph, ast, output));
    if (node.assignedValue) {
      node.assignedValueId = getOrAddValue(graph, ast, node.assignedValue);
      node.assignedFromId = getOrAddValue(graph, ast, node.assignedFrom);
    }
    if (node.kind == SimNodeKind::Assert) {
      if (node.inputIds.size() != 1)
        return node.op->emitError("simulation assertion requires one condition");
      node.assertionConditionId = node.inputIds.front();
    }
    if (node.kind == SimNodeKind::Return)
      graph.outputValueIds = node.inputIds;
    if (auto it = graph.expressionIds.find(node.op);
        it != graph.expressionIds.end())
      node.expressionIds.push_back(it->second);
  }
  return success();
}

static FailureOr<unsigned> captureCombRegion(
    SimGraph &graph, const SimulationAST &ast,
    const SimulationASTNode &comb) {
  if (comb.kind != SimNodeKind::CombRegion || comb.regions.size() != 1 ||
      comb.regions.front().blocks.size() != 1) {
    comb.source->emitError("simulation comb region requires one block");
    return failure();
  }
  const SimulationASTBlock &body = comb.regions.front().blocks.front();
  if (!body.childCount ||
      body.firstChild + body.childCount > comb.children.size() ||
      comb.children[body.firstChild + body.childCount - 1].kind !=
          SimNodeKind::Yield) {
    comb.source->emitError("simulation comb region requires a yield");
    return failure();
  }
  const SimulationASTNode &yield =
      comb.children[body.firstChild + body.childCount - 1];
  unsigned regionId = graph.combRegions.size();
  graph.combRegions.emplace_back();
  SimCombRegion region;
  region.source = comb.source;
  for (Value value : comb.inputs)
    region.inputIds.push_back(getOrAddValue(graph, ast, value));
  for (Value value : body.arguments)
    region.argumentIds.push_back(getOrAddValue(graph, ast, value));
  for (Value value : comb.outputs)
    region.resultIds.push_back(getOrAddValue(graph, ast, value));
  for (Value value : yield.inputs)
    region.yieldIds.push_back(getOrAddValue(graph, ast, value));
  for (unsigned childIndex = body.firstChild;
       childIndex + 1 < body.firstChild + body.childCount; ++childIndex) {
    const SimulationASTNode &step = comb.children[childIndex];
    if (step.kind == SimNodeKind::CombRegion) {
      FailureOr<unsigned> nestedId = captureCombRegion(graph, ast, step);
      if (failed(nestedId))
        return failure();
      region.steps.push_back(SimCombStep{~0u, *nestedId});
    } else if (auto it = graph.expressionIds.find(step.source);
               it != graph.expressionIds.end()) {
      region.steps.push_back(SimCombStep{it->second, ~0u});
    } else {
      step.source->emitError("simulation comb step has no graph expression");
      return failure();
    }
  }
  graph.combRegions[regionId] = std::move(region);
  return regionId;
}

static LogicalResult captureCombRegions(SimGraph &graph,
                                        const SimulationAST &ast) {
  for (auto [index, node] : llvm::enumerate(ast.body)) {
    if (node.kind != SimNodeKind::CombRegion)
      continue;
    FailureOr<unsigned> regionId = captureCombRegion(graph, ast, node);
    if (failed(regionId))
      return failure();
    graph.topNodes[index].combRegionId = *regionId;
  }
  return success();
}

static SimNodeKind classifyNode(Operation *op) {
  if (isFusableSimCombOp(op)) return SimNodeKind::Expression;
  if (isa<pyc::CombOp>(op)) return SimNodeKind::CombRegion;
  if (isa<pyc::YieldOp>(op)) return SimNodeKind::Yield;
  if (isa<pyc::WireOp>(op)) return SimNodeKind::Wire;
  if (isa<pyc::AssignOp>(op)) return SimNodeKind::Assign;
  if (isa<pyc::AssertOp>(op)) return SimNodeKind::Assert;
  if (isa<func::ReturnOp>(op)) return SimNodeKind::Return;
  if (isa<pyc::RegOp>(op)) return SimNodeKind::Reg;
  if (isa<pyc::FifoOp>(op)) return SimNodeKind::Fifo;
  if (isa<pyc::ByteMemOp>(op)) return SimNodeKind::ByteMem;
  if (isa<pyc::SyncMemOp>(op)) return SimNodeKind::SyncMem;
  if (isa<pyc::SyncMemDPOp>(op)) return SimNodeKind::SyncMemDP;
  if (isa<pyc::AsyncFifoOp>(op)) return SimNodeKind::AsyncFifo;
  if (isa<pyc::CdcSyncOp>(op)) return SimNodeKind::CdcSync;
  if (isa<pyc::InstanceOp>(op)) return SimNodeKind::Instance;
  return SimNodeKind::Unknown;
}

static std::string capturePortPath(func::FuncOp function, unsigned index,
                                   bool result);

static SimNode makeNode(const SimulationASTNode &astNode) {
  Operation *op = astNode.source;
  SimNode node;
  node.kind = astNode.kind;
  node.sourceOperationName = astNode.opcode;
  node.op = op;
  node.operations.push_back(op);
  node.inputs = astNode.inputs;
  node.outputs = astNode.outputs;
  node.cdcStages = astNode.cdcStages;
  node.primitiveDepth = astNode.primitiveDepth;
  node.primitiveName = astNode.primitiveName;
  node.primitiveHasName = astNode.primitiveHasName;
  node.instanceCallee = astNode.instanceCallee;
  node.instanceInputPortPaths = astNode.instanceInputPortPaths;
  node.instanceOutputPortPaths = astNode.instanceOutputPortPaths;
  node.instanceCalleeResolved = astNode.instanceCalleeResolved;
  node.instanceName = astNode.instanceName;
  node.instanceShortName = astNode.instanceShortName;
  node.instanceHasName = astNode.instanceHasName;
  node.instanceHasShortName = astNode.instanceHasShortName;
  node.assertionMessage = astNode.assertionMessage;
  if (node.kind == SimNodeKind::Assign && node.inputs.size() == 2) {
    node.assignedValue = node.inputs[0];
    node.assignedFrom = node.inputs[1];
  }
  node.stateBoundary = astNode.stateBoundary;
  node.observable = node.kind == SimNodeKind::Assert ||
                    node.kind == SimNodeKind::Return;
  return node;
}

static void appendFineNodes(const SimulationASTNode &node,
                            llvm::SmallVectorImpl<SimNode> &fineNodes) {
  for (const SimulationASTNode &child : node.children)
    appendFineNodes(child, fineNodes);
  fineNodes.push_back(makeNode(node));
}

static void gatherExpressionDescriptions(
    const SimulationASTNode &node,
    llvm::DenseMap<Operation *, const SimulationASTExpression *> &out) {
  if (node.expression)
    out.try_emplace(node.source, &*node.expression);
  for (const SimulationASTNode &child : node.children)
    gatherExpressionDescriptions(child, out);
}

static std::string capturePortPath(func::FuncOp function, unsigned index,
                                   bool result) {
  auto names = function->getAttrOfType<ArrayAttr>(result ? "result_names" : "arg_names");
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

struct DerivedConnectivity {
  llvm::SmallVector<SimEdge> edges;
  bool hasAmbiguousDrivers = false;
};

// Derive the complete edge set from graph nodes. Keeping this independent of
// the stored edges lets the verifier catch a graph pass that leaves stale or
// missing dependencies before scheduling uses them.
static DerivedConnectivity deriveConnectivity(const SimGraph &graph) {
  DerivedConnectivity result;
  llvm::DenseMap<unsigned, unsigned> producer;
  llvm::DenseMap<unsigned, unsigned> assignCount;
  llvm::DenseMap<unsigned, unsigned> wireAssign;
  for (auto [i, node] : llvm::enumerate(graph.topNodes)) {
    for (unsigned value : node.outputIds)
      producer.try_emplace(value, static_cast<unsigned>(i));
    if (node.assignedValue && ++assignCount[node.assignedValueId] == 1)
      wireAssign[node.assignedValueId] = static_cast<unsigned>(i);
  }
  for (const auto &entry : assignCount)
    result.hasAmbiguousDrivers |= entry.second > 1;
  for (const auto &entry : wireAssign)
    if (assignCount.lookup(entry.first) == 1)
      producer[entry.first] = entry.second;

  for (auto [i, node] : llvm::enumerate(graph.topNodes)) {
    auto addEdge = [&](unsigned value) {
      auto it = producer.find(value);
      if (it == producer.end() || it->second == i)
        return;
      const SimNode &from = graph.topNodes[it->second];
      result.edges.push_back(SimEdge{it->second, static_cast<unsigned>(i), value,
                                     graph.values[value].source,
                                     from.stateBoundary || node.stateBoundary,
                                     node.observable});
    };
    if (node.assignedValue)
      addEdge(node.assignedFromId);
    else
      for (unsigned value : node.inputIds)
        addEdge(value);
  }
  return result;
}

} // namespace

FailureOr<SimGraph> buildSimGraph(const SimulationAST &ast) {
  if (failed(ast.verify()))
    return failure();
  SimGraph graph;
  graph.function = ast.function;
  graph.functionName = ast.functionName;
  graph.structuralEmission = ast.structuralEmission;
  llvm::DenseMap<Operation *, const SimulationASTExpression *> descriptions;
  for (const SimulationASTNode &node : ast.body) {
    graph.topNodes.push_back(makeNode(node));
    appendFineNodes(node, graph.fineNodes);
    gatherExpressionDescriptions(node, descriptions);
  }
  if (failed(captureExpressions(graph, ast, descriptions)))
    return failure();
  if (failed(captureCombRegions(graph, ast)))
    return failure();
  func::FuncOp function = ast.function;
  for (auto [i, input] : llvm::enumerate(function.getArguments())) {
    graph.inputValueIds.push_back(getOrAddValue(graph, ast, input));
    graph.inputPortPaths.push_back(ast.inputPortPaths[i]);
  }
  graph.outputPortPaths = ast.outputPortPaths;
  for (Value result : ast.declarationValues)
    graph.declarationValueIds.push_back(getOrAddValue(graph, ast, result));
  for (const SimNode &node : graph.fineNodes)
    if (node.outputIds.size() == 1 &&
        graph.values[node.outputIds.front()].sourceNameIsExplicit)
      graph.namedProbeValueIds.push_back(node.outputIds.front());
  if (failed(graph.rebuildEdges()) || failed(graph.verify()))
    return failure();
  return graph;
}

LogicalResult SimGraph::rebuildEdges() {
  for (SimValue &value : values)
    value.expressionUsers.clear();
  for (auto [id, expr] : llvm::enumerate(expressions))
    for (unsigned operand : expr.operands) {
      if (operand >= values.size())
        return function.emitError("simulation expression has an invalid operand ID");
      values[operand].expressionUsers.push_back(static_cast<unsigned>(id));
    }
  DerivedConnectivity derived = deriveConnectivity(*this);
  edges = std::move(derived.edges);
  hasAmbiguousDrivers = derived.hasAmbiguousDrivers;
  return success();
}

LogicalResult SimGraph::verify() {
  if (!function)
    return failure();
  if (functionName != function.getSymName().str())
    return function.emitError("simulation graph function name is stale");
  if (structuralEmission != captureStructuralEmission(function))
    return function.emitError("simulation graph emission mode is stale");
  if (!llvm::hasSingleElement(function.getBody()))
    return function.emitError("simulation graph requires a single-block function");
  if (inputValueIds.size() != function.getNumArguments() ||
      inputPortPaths.size() != inputValueIds.size() ||
      outputPortPaths.size() != function.getNumResults() ||
      outputValueIds.size() != outputPortPaths.size())
    return function.emitError("simulation graph port metadata is incomplete");
  for (auto [i, arg] : llvm::enumerate(function.getArguments()))
    if (inputValueIds[i] >= values.size() || values[inputValueIds[i]].source != arg ||
        inputPortPaths[i] != capturePortPath(function, i, false))
      return function.emitError("simulation graph input port metadata is stale");
  for (unsigned i = 0; i < outputPortPaths.size(); ++i)
    if (outputPortPaths[i] != capturePortPath(function, i, true))
      return function.emitError("simulation graph output port metadata is stale");
  unsigned declarationIndex = 0;
  bool declarationMismatch = false;
  function.walk([&](Operation *op) {
    for (Value result : op->getResults()) {
      if (declarationIndex >= declarationValueIds.size() ||
          declarationValueIds[declarationIndex] >= values.size() ||
          values[declarationValueIds[declarationIndex]].source != result)
        declarationMismatch = true;
      ++declarationIndex;
    }
  });
  if (declarationMismatch || declarationIndex != declarationValueIds.size())
    return function.emitError("simulation graph declarations are stale");
  llvm::DenseSet<Operation *> represented;
  llvm::DenseSet<unsigned> representedCombRegions;
  llvm::DenseSet<unsigned> inlinedValues;
  for (const SimExpr &expr : expressions)
    if (expr.inlineIntoConsumer)
      inlinedValues.insert(expr.result);
  for (const SimNode &node : topNodes) {
    if (!node.op || node.operations.empty() || node.operations.front() != node.op)
      return function.emitError("simulation graph has a node without a source operation");
    if (node.kind != SimNodeKind::PureGroup &&
        node.sourceOperationName != node.op->getName().getStringRef())
      return node.op->emitError("simulation graph operation name is stale");
    if (node.kind == SimNodeKind::Unknown)
      return node.op->emitError("unsupported simulation graph node");
    if (node.kind == SimNodeKind::PureGroup) {
      if (!llvm::all_of(node.operations, isFusableSimCombOp))
        return node.op->emitError("simulation pure group contains an effectful operation");
    } else if (node.kind != classifyNode(node.op)) {
      return node.op->emitError("simulation node kind is stale");
    }
    if (node.inputIds.size() != node.inputs.size() ||
        node.outputIds.size() != node.outputs.size())
      return node.op->emitError("simulation graph value IDs are incomplete");
    for (auto [value, id] : llvm::zip(node.inputs, node.inputIds))
      if (id >= values.size() || values[id].source != value)
        return node.op->emitError("simulation graph input ID is stale");
    for (auto [value, id] : llvm::zip(node.outputs, node.outputIds))
      if (id >= values.size() || values[id].source != value)
        return node.op->emitError("simulation graph output ID is stale");
    if (isa<pyc::RegOp>(node.op)) {
      if (node.inputIds.size() != 5 || node.outputIds.size() != 1)
        return node.op->emitError("simulation register bindings are incomplete");
      const SimType &next = values[node.inputIds[3]].type;
      const SimType &init = values[node.inputIds[4]].type;
      const SimType &q = values[node.outputIds.front()].type;
      if (!q.width || next.width != q.width || init.width != q.width ||
          next.shape != q.shape || init.shape != q.shape ||
          values[node.inputIds[0]].type.width != 1 ||
          values[node.inputIds[1]].type.width != 1 ||
          values[node.inputIds[2]].type.width != 1)
        return node.op->emitError("simulation register type bindings are invalid");
    }
    if (node.kind == SimNodeKind::CdcSync) {
      uint64_t expectedStages = 2;
      if (auto attr = node.op->getAttrOfType<IntegerAttr>("stages"))
        expectedStages = attr.getValue().getZExtValue();
      if (node.cdcStages != expectedStages || node.cdcStages == 0 ||
          node.inputIds.size() != 3 || node.outputIds.size() != 1 ||
          values[node.inputIds[0]].type.width != 1 ||
          values[node.inputIds[1]].type.width != 1 ||
          values[node.inputIds[2]].type.width !=
              values[node.outputIds.front()].type.width ||
          !values[node.outputIds.front()].type.shape.empty())
        return node.op->emitError("simulation CDC binding is invalid");
    } else if (node.cdcStages != 0) {
      return node.op->emitError("non-CDC node has CDC stage metadata");
    }
    if (node.kind == SimNodeKind::Fifo ||
        node.kind == SimNodeKind::AsyncFifo) {
      bool async = node.kind == SimNodeKind::AsyncFifo;
      auto depth = node.op->getAttrOfType<IntegerAttr>("depth");
      if (!depth || node.primitiveDepth != depth.getValue().getZExtValue() ||
          node.primitiveDepth == 0 ||
          node.inputIds.size() != (async ? 7u : 5u) ||
          node.outputIds.size() != 3 ||
          values[node.inputIds[async ? 5 : 3]].type.width !=
              values[node.outputIds[2]].type.width ||
          !values[node.inputIds[async ? 5 : 3]].type.shape.empty() ||
          !values[node.outputIds[2]].type.shape.empty())
        return node.op->emitError("simulation FIFO binding is invalid");
    } else if (node.kind == SimNodeKind::ByteMem ||
               node.kind == SimNodeKind::SyncMem ||
               node.kind == SimNodeKind::SyncMemDP) {
      auto depth = node.op->getAttrOfType<IntegerAttr>("depth");
      auto name = node.op->getAttrOfType<StringAttr>("name");
      unsigned inputCount = node.kind == SimNodeKind::ByteMem ? 7u :
                            node.kind == SimNodeKind::SyncMem ? 8u : 10u;
      unsigned outputCount = node.kind == SimNodeKind::SyncMemDP ? 2u : 1u;
      if (!depth || node.primitiveDepth != depth.getValue().getZExtValue() ||
          node.primitiveDepth == 0 || node.inputIds.size() != inputCount ||
          node.outputIds.size() != outputCount ||
          node.primitiveHasName != static_cast<bool>(name) ||
          node.primitiveName != (name ? name.getValue().str() : ""))
        return node.op->emitError("simulation memory binding is invalid");
      unsigned readAddress = node.kind == SimNodeKind::ByteMem ? 2u : 3u;
      unsigned writeAddress = node.kind == SimNodeKind::ByteMem ? 4u :
                              node.kind == SimNodeKind::SyncMem ? 5u : 7u;
      unsigned writeData = node.kind == SimNodeKind::ByteMem ? 5u :
                           node.kind == SimNodeKind::SyncMem ? 6u : 8u;
      const SimType &address = values[node.inputIds[readAddress]].type;
      const SimType &data = values[node.outputIds.front()].type;
      if (!address.width || !data.width || !address.shape.empty() ||
          !data.shape.empty() ||
          values[node.inputIds[writeAddress]].type.width != address.width ||
          !values[node.inputIds[writeAddress]].type.shape.empty() ||
          values[node.inputIds[writeData]].type.width != data.width ||
          !values[node.inputIds[writeData]].type.shape.empty())
        return node.op->emitError("simulation memory type binding is invalid");
      if (node.kind == SimNodeKind::SyncMemDP &&
          (values[node.inputIds[5]].type.width != address.width ||
           values[node.outputIds[1]].type.width != data.width))
        return node.op->emitError("simulation dual-read memory type binding is invalid");
    } else if (node.primitiveDepth != 0) {
      return node.op->emitError("non-memory node has depth metadata");
    }
    if (node.kind != SimNodeKind::ByteMem &&
        node.kind != SimNodeKind::SyncMem &&
        node.kind != SimNodeKind::SyncMemDP &&
        (node.primitiveHasName || !node.primitiveName.empty())) {
      return node.op->emitError("non-memory node has memory name metadata");
    }
    if (node.kind == SimNodeKind::Instance) {
      auto callee = node.op->getAttrOfType<FlatSymbolRefAttr>("callee");
      ModuleOp module = function->getParentOfType<ModuleOp>();
      func::FuncOp calleeFunction =
          callee && module
              ? module.lookupSymbol<func::FuncOp>(callee.getValue())
              : func::FuncOp();
      auto name = node.op->getAttrOfType<StringAttr>("name");
      auto shortName = node.op->getAttrOfType<StringAttr>("short_name");
      if (!callee || node.instanceCallee != callee.getValue() ||
          !calleeFunction || !node.instanceCalleeResolved ||
          node.instanceInputPortPaths.size() !=
              calleeFunction.getNumArguments() ||
          node.instanceOutputPortPaths.size() !=
              calleeFunction.getNumResults() ||
          node.instanceHasName != static_cast<bool>(name) ||
          node.instanceName != (name ? name.getValue().str() : "") ||
          node.instanceHasShortName != static_cast<bool>(shortName) ||
          node.instanceShortName !=
              (shortName ? shortName.getValue().str() : ""))
        return node.op->emitError("simulation instance metadata is stale");
      for (unsigned i = 0; i < calleeFunction.getNumArguments(); ++i)
        if (node.instanceInputPortPaths[i] !=
            capturePortPath(calleeFunction, i, false))
          return node.op->emitError("simulation instance input port is stale");
      for (unsigned i = 0; i < calleeFunction.getNumResults(); ++i)
        if (node.instanceOutputPortPaths[i] !=
            capturePortPath(calleeFunction, i, true))
          return node.op->emitError("simulation instance output port is stale");
    } else if (!node.instanceCallee.empty() || node.instanceHasName ||
               node.instanceCalleeResolved ||
               !node.instanceInputPortPaths.empty() ||
               !node.instanceOutputPortPaths.empty() ||
               !node.instanceName.empty() || node.instanceHasShortName ||
               !node.instanceShortName.empty()) {
      return node.op->emitError("non-instance node has instance metadata");
    }
    if (isa<pyc::AssignOp>(node.op) != static_cast<bool>(node.assignedValue))
      return node.op->emitError("simulation assignment presence is stale");
    if (node.assignedValue &&
        (node.assignedValueId >= values.size() ||
         values[node.assignedValueId].source != node.assignedValue ||
         node.assignedFromId >= values.size() ||
         values[node.assignedFromId].source != node.assignedFrom))
      return node.op->emitError("simulation assignment IDs are stale");
    if (!node.assignedValue &&
        (node.assignedValueId != ~0u || node.assignedFromId != ~0u))
      return node.op->emitError("non-assign node has assignment metadata");
    if (auto assertion = dyn_cast<pyc::AssertOp>(node.op)) {
      std::string expectedMessage = assertion.getMsgAttr()
                                        ? assertion.getMsgAttr().getValue().str()
                                        : "pyc.assert failed";
      if (node.assertionConditionId >= values.size() ||
          node.inputIds.size() != 1 ||
          node.assertionConditionId != node.inputIds.front() ||
          values[node.assertionConditionId].source != assertion.getCond() ||
          node.assertionMessage != expectedMessage)
        return assertion.emitError("simulation assertion description is stale");
    } else if (node.assertionConditionId != ~0u ||
               !node.assertionMessage.empty()) {
      return node.op->emitError("non-assert node has assertion metadata");
    }
    if ((node.kind == SimNodeKind::PureGroup ||
         node.kind == SimNodeKind::Expression) &&
        node.expressionIds.size() != node.operations.size())
      return node.op->emitError("simulation expression mapping is incomplete");
    for (auto [op, id] : llvm::zip(node.operations, node.expressionIds))
      if (id >= expressions.size() || expressions[id].source != op)
        return node.op->emitError("simulation expression mapping is stale");
    for (unsigned id : node.replicatedExpressionIds)
      if (id >= expressions.size() || expressions[id].source ||
          !expressions[id].inlineIntoConsumer ||
          expressions[id].result >= values.size() ||
          values[expressions[id].result].source)
        return node.op->emitError("simulation replicated expression is invalid");
    unsigned previousBatchEnd = 0;
    for (const MuxConditionBatch &batch : node.muxConditionBatches) {
      if (batch.begin < previousBatchEnd || batch.end > node.expressionIds.size() ||
          batch.end - batch.begin <= 5 || batch.selectorId >= values.size() ||
          values[batch.selectorId].type.width != 1 ||
          !values[batch.selectorId].type.shape.empty())
        return node.op->emitError("simulation mux condition batch is invalid");
      for (unsigned index = batch.begin; index < batch.end; ++index) {
        const SimExpr &expr = expressions[node.expressionIds[index]];
        if (expr.kind != SimExprKind::Mux || expr.operands.size() != 3 ||
            expr.operands[0] != batch.selectorId ||
            expr.result >= values.size() ||
            expr.inlineIntoConsumer || expr.omitOriginalEvaluation ||
            !values[expr.result].type.shape.empty() ||
            !values[expr.result].source)
          return node.op->emitError("simulation mux condition step is invalid");
        for (unsigned operand : expr.operands)
          if (operand >= values.size() || !values[operand].source ||
              inlinedValues.contains(operand))
            return node.op->emitError("simulation mux condition operand is invalid");
      }
      previousBatchEnd = batch.end;
    }
    if (!node.activationUsedBits.empty() &&
        (!node.activateOnInputChange ||
         node.activationUsedBits.size() != node.inputs.size()))
      return node.op->emitError("simulation bit demand has an invalid input mapping");
    for (auto [input, mask] : llvm::zip(node.inputs, node.activationUsedBits)) {
      unsigned width = isa<IntegerType>(input.getType())
                           ? cast<IntegerType>(input.getType()).getWidth()
                           : 1;
      if (mask.getBitWidth() != width ||
          (!isa<IntegerType>(input.getType()) && !mask.isAllOnes()))
        return node.op->emitError("simulation bit demand has an invalid mask");
    }
    if (!node.activationVectorLanes.empty() &&
        (!node.activateOnInputChange ||
         node.activationVectorLanes.size() != node.inputIds.size()))
      return node.op->emitError("simulation vector-lane demand has an invalid input mapping");
    for (auto [id, mask] :
         llvm::zip(node.inputIds, node.activationVectorLanes)) {
      const SimType &type = values[id].type;
      unsigned lanes = type.shape.empty()
                           ? 1u
                           : static_cast<unsigned>(type.shape.front());
      if (mask.getBitWidth() != lanes ||
          (type.shape.empty() && !mask.isAllOnes()))
        return node.op->emitError("simulation vector-lane demand has an invalid mask");
    }
    if (!node.activationVectorElements.empty() &&
        (!node.activateOnInputChange ||
         node.activationVectorElements.size() != node.inputIds.size()))
      return node.op->emitError("simulation vector-element demand has an invalid input mapping");
    for (auto [id, mask] :
         llvm::zip(node.inputIds, node.activationVectorElements)) {
      const SimType &type = values[id].type;
      uint64_t count = type.shape.size() == 2
                           ? static_cast<uint64_t>(type.shape[0]) *
                                 static_cast<uint64_t>(type.shape[1])
                           : 0;
      unsigned expected = count && count <= std::numeric_limits<unsigned>::max()
                              ? static_cast<unsigned>(count) : 1u;
      if (mask.getBitWidth() != expected ||
          (expected == 1 && !mask.isAllOnes()))
        return node.op->emitError("simulation vector-element demand has an invalid mask");
    }
    for (Operation *op : node.operations)
      if (!represented.insert(op).second)
        return op->emitError("simulation graph represents an operation more than once");
    if (node.operations.size() > 1) {
      llvm::DenseSet<unsigned> produced;
      llvm::DenseSet<unsigned> expectedInputs;
      llvm::DenseSet<Value> expectedOutputs;
      llvm::DenseMap<unsigned, unsigned> producerOrder;
      unsigned expressionOrder = 0;
      for (unsigned id : node.replicatedExpressionIds)
        producerOrder.try_emplace(expressions[id].result, expressionOrder++);
      for (unsigned id : node.expressionIds)
        producerOrder.try_emplace(expressions[id].result, expressionOrder++);
      if (producerOrder.size() != expressionOrder)
        return node.op->emitError("simulation group defines a value more than once");
      for (const auto &entry : producerOrder)
        produced.insert(entry.first);
      auto validInternalOrder = [&](unsigned id, unsigned order) {
        for (unsigned operand : expressions[id].operands)
          if (auto it = producerOrder.find(operand);
              it != producerOrder.end() && it->second >= order)
            return false;
        return true;
      };
      expressionOrder = 0;
      for (unsigned id : node.replicatedExpressionIds)
        if (!validInternalOrder(id, expressionOrder++))
          return node.op->emitError("simulation group has a non-topological replicated expression");
      for (unsigned id : node.expressionIds)
        if (!validInternalOrder(id, expressionOrder++))
          return node.op->emitError("simulation group has a non-topological expression");
      auto collectInputs = [&](unsigned id) {
        for (unsigned operand : expressions[id].operands)
          if (!produced.contains(operand))
            expectedInputs.insert(operand);
      };
      for (unsigned id : node.replicatedExpressionIds)
        collectInputs(id);
      for (unsigned id : node.expressionIds)
        collectInputs(id);
      for (Operation *op : node.operations) {
        if (!isFusableSimCombOp(op))
          return op->emitError("simulation group contains a non-combinational operation");
        for (Value value : op->getResults())
          expectedOutputs.insert(value);
      }
      if (expectedInputs.size() != node.inputs.size() ||
          expectedOutputs.size() != node.outputs.size())
        return node.op->emitError("simulation group input/output mapping is incomplete");
      for (unsigned id : node.inputIds)
        if (!expectedInputs.contains(id))
          return node.op->emitError("simulation group has an invalid input mapping");
      for (Value value : node.outputs)
        if (!expectedOutputs.contains(value))
          return node.op->emitError("simulation group has an invalid output mapping");
    }
    if (auto comb = dyn_cast<pyc::CombOp>(node.op)) {
      if (node.combRegionId >= combRegions.size() ||
          combRegions[node.combRegionId].source != node.op ||
          combRegions[node.combRegionId].inputIds != node.inputIds ||
          combRegions[node.combRegionId].resultIds != node.outputIds ||
          !representedCombRegions.insert(node.combRegionId).second)
        return comb.emitError("simulation comb region ID is invalid");
    } else if (node.combRegionId != ~0u) {
      return node.op->emitError("non-comb node owns a comb region");
    }
  }
  for (auto [regionId, region] : llvm::enumerate(combRegions)) {
    auto comb = dyn_cast_or_null<pyc::CombOp>(region.source);
    if (!comb || comb.getBody().empty() ||
        !llvm::hasSingleElement(comb.getBody()))
      return function.emitError("simulation comb region source is invalid");
    Block &body = comb.getBody().front();
    auto yield = dyn_cast_or_null<pyc::YieldOp>(body.getTerminator());
    if (body.getNumArguments() != comb.getNumOperands() || !yield ||
        yield.getNumOperands() != comb.getNumResults() ||
        region.inputIds.size() != comb.getNumOperands() ||
        region.resultIds.size() != comb.getNumResults() ||
        region.argumentIds.size() != body.getNumArguments() ||
        region.yieldIds.size() != yield.getNumOperands() ||
        region.steps.size() + 1 != body.getOperations().size())
      return comb.emitError("simulation comb region mapping is incomplete");
    for (auto [id, value] : llvm::zip(region.inputIds, comb.getInputs()))
      if (id >= values.size() || values[id].source != value)
        return comb.emitError("simulation comb input ID is stale");
    for (auto [id, value] : llvm::zip(region.resultIds, comb.getResults()))
      if (id >= values.size() || values[id].source != value)
        return comb.emitError("simulation comb result ID is stale");
    for (auto [id, value] : llvm::zip(region.argumentIds,
                                      body.getArguments()))
      if (id >= values.size() || values[id].source != value)
        return comb.emitError("simulation comb argument ID is stale");
    for (auto [id, value] : llvm::zip(region.yieldIds, yield.getValues()))
      if (id >= values.size() || values[id].source != value)
        return comb.emitError("simulation comb yield ID is stale");
    unsigned stepIndex = 0;
    for (Operation &op : body) {
      if (&op == yield.getOperation())
        break;
      const SimCombStep &step = region.steps[stepIndex++];
      if (step.expressionId != ~0u) {
        if (step.expressionId >= expressions.size() ||
            expressions[step.expressionId].source != &op ||
            step.nestedRegionId != ~0u)
          return op.emitError("simulation comb expression step is stale");
      } else if (step.nestedRegionId >= combRegions.size() ||
                 combRegions[step.nestedRegionId].source != &op ||
                 step.nestedRegionId <= regionId ||
                 !representedCombRegions.insert(step.nestedRegionId).second) {
        return op.emitError("simulation comb nested region is stale");
      }
    }
  }
  if (representedCombRegions.size() != combRegions.size())
    return function.emitError("simulation graph omitted a comb region");
  for (Operation &op : function.getBody().front()) {
    if (represented.contains(&op))
      continue;
    auto expression = expressionIds.find(&op);
    if (expression != expressionIds.end() &&
        expressions[expression->second].omitOriginalEvaluation)
      continue;
    return op.emitError("simulation graph omitted an operation");
  }
  llvm::SmallVector<llvm::SmallVector<unsigned, 2>> expectedUsers(values.size());
  for (auto [id, expr] : llvm::enumerate(expressions))
    for (unsigned operand : expr.operands) {
      if (operand >= values.size())
        return function.emitError("simulation expression has an invalid operand");
      expectedUsers[operand].push_back(static_cast<unsigned>(id));
    }
  for (auto [value, users] : llvm::zip(values, expectedUsers))
    if (value.expressionUsers != users)
      return function.emitError("simulation expression use-def links are stale");
  for (const SimValue &value : values) {
    if (value.sourceBacked != static_cast<bool>(value.source))
      return function.emitError("simulation value source flag is stale");
    if (!value.source) {
      if (!value.sourceNameBase.empty() || value.sourceNameIsExplicit)
        return function.emitError("synthetic simulation value has source name metadata");
      continue;
    }
    Operation *def = value.source.getDefiningOp();
    auto name = def ? def->getAttrOfType<StringAttr>("pyc.name") : StringAttr();
    std::string expected = name ? name.getValue().str()
                                : def ? def->getName().getStringRef().str() : "";
    if (value.sourceNameIsExplicit != static_cast<bool>(name) ||
        value.sourceNameBase != expected)
      return function.emitError("simulation value name metadata is stale");
  }
  llvm::DenseMap<unsigned, unsigned> expressionProducer;
  for (auto [id, expr] : llvm::enumerate(expressions))
    if (!expressionProducer.try_emplace(expr.result,
                                        static_cast<unsigned>(id)).second)
      return function.emitError("simulation value has multiple expression producers");
  llvm::SmallVector<unsigned> indegree(expressions.size(), 0);
  llvm::SmallVector<llvm::SmallVector<unsigned, 2>> successors(expressions.size());
  for (auto [id, expr] : llvm::enumerate(expressions))
    for (unsigned operand : expr.operands)
      if (auto it = expressionProducer.find(operand);
          it != expressionProducer.end()) {
        ++indegree[id];
        successors[it->second].push_back(static_cast<unsigned>(id));
      }
  llvm::SmallVector<unsigned> ready;
  for (unsigned id = 0; id < expressions.size(); ++id)
    if (!indegree[id])
      ready.push_back(id);
  unsigned visitedExpressions = 0;
  while (!ready.empty()) {
    unsigned id = ready.pop_back_val();
    ++visitedExpressions;
    for (unsigned user : successors[id])
      if (--indegree[user] == 0)
        ready.push_back(user);
  }
  if (visitedExpressions != expressions.size())
    return function.emitError("simulation expression DAG contains a cycle");
  llvm::DenseSet<unsigned> graphUses;
  for (const SimNode &node : topNodes)
    graphUses.insert(node.inputIds.begin(), node.inputIds.end());
  for (const SimExpr &expr : expressions)
    graphUses.insert(expr.operands.begin(), expr.operands.end());
  for (const SimExpr &expr : expressions) {
    if (expr.result >= values.size() || !values[expr.result].type.width)
      return function.emitError("simulation expression has an invalid result");
    for (unsigned input : expr.operands)
      if (input >= values.size() || !values[input].type.width)
        return function.emitError("simulation expression has an invalid operand");
    if (expr.kind == SimExprKind::Constant &&
        expr.constant.getBitWidth() != values[expr.result].type.width)
      return function.emitError("simulation constant has an invalid width");
    if (expr.kind == SimExprKind::VBroadcastDim) {
      if (expr.operands.size() != 1 ||
          expr.operands.front() >= values.size())
        return function.emitError("simulation vector broadcast has an invalid input");
      const auto &sourceShape = values[expr.operands.front()].type.shape;
      const auto &resultShape = values[expr.result].type.shape;
      if (expr.dimension < 0 ||
          static_cast<size_t>(expr.dimension) >= resultShape.size() ||
          resultShape.size() != sourceShape.size() + 1 ||
          resultShape[expr.dimension] != expr.immediate)
        return function.emitError("simulation vector broadcast shape is invalid");
      for (size_t axis = 0, sourceAxis = 0; axis < resultShape.size(); ++axis)
        if (static_cast<int64_t>(axis) != expr.dimension &&
            resultShape[axis] != sourceShape[sourceAxis++])
          return function.emitError("simulation vector broadcast dimensions are stale");
    }
    if (expr.omitOriginalEvaluation) {
      if (values[expr.result].observable || graphUses.contains(expr.result))
        return function.emitError("simulation omitted an observable expression");
    }
  }
  for (auto [id, expr] : llvm::enumerate(expressions))
    if (expr.source && expr.sourceOrder != id)
      return function.emitError("simulation source expression order is stale");
  if (!valueUsedBits.empty()) {
    if (valueUsedBits.size() != values.size())
      return function.emitError("simulation bit demand does not cover graph values");
    for (auto [value, bits] : llvm::zip(values, valueUsedBits)) {
      unsigned width = value.type.shape.empty() && value.type.width
                           ? value.type.width : 1;
      if (bits.getBitWidth() != width)
        return function.emitError("simulation bit demand has an invalid value width");
    }
  }
  for (const SimNode &node : fineNodes) {
    if (node.inputIds.size() != node.inputs.size() ||
        node.outputIds.size() != node.outputs.size())
      return node.op->emitError("simulation fine node value IDs are incomplete");
    for (auto [value, id] : llvm::zip(node.inputs, node.inputIds))
      if (id >= values.size() || values[id].source != value)
        return node.op->emitError("simulation fine node input ID is stale");
    for (auto [value, id] : llvm::zip(node.outputs, node.outputIds))
      if (id >= values.size() || values[id].source != value)
        return node.op->emitError("simulation fine node output ID is stale");
    if (isFusableSimCombOp(node.op) && !expressionIds.count(node.op))
      return node.op->emitError("simulation graph omitted a pure expression");
  }
  llvm::DenseSet<unsigned> groupedIndices;
  for (unsigned index : groupedNodes) {
    if (index >= topNodes.size() || !groupedIndices.insert(index).second ||
        topNodes[index].expressionIds.empty() ||
        (topNodes[index].kind != SimNodeKind::Expression &&
         topNodes[index].kind != SimNodeKind::PureGroup))
      return function.emitError("simulation graph has an invalid group index");
    const SimNode &group = topNodes[index];
    llvm::DenseMap<unsigned, unsigned> internalUses;
    for (unsigned id : group.expressionIds)
      for (unsigned operand : expressions[id].operands)
        ++internalUses[operand];
    for (unsigned id : group.expressionIds) {
      const SimExpr &expr = expressions[id];
      if (!expr.inlineIntoConsumer)
        continue;
      const SimValue &value = values[expr.result];
      unsigned uses = internalUses.lookup(expr.result);
      if (value.observable || !uses || value.sourceUseCount != uses)
        return function.emitError("simulation inline expression has an external use");
    }
  }
  for (auto [index, node] : llvm::enumerate(topNodes))
    if (node.activateOnInputChange && !groupedIndices.contains(index))
      return node.op->emitError("simulation activation lacks a graph group");
  auto returned = dyn_cast_or_null<func::ReturnOp>(
      function.getBody().front().getTerminator());
  if (!returned || outputValueIds.size() != returned.getNumOperands() ||
      outputValueIds.size() != function.getNumResults())
    return function.emitError("simulation output roots are incomplete");
  for (auto [index, id] : llvm::enumerate(outputValueIds))
    if (id >= values.size() ||
        values[id].source != returned.getOperand(index))
      return function.emitError("simulation output root ID is stale");
  llvm::SmallVector<unsigned> expectedNamedProbes;
  bool missingNamedValue = false;
  size_t fineIndex = 0;
  bool fineMismatch = false;
  function.walk([&](Operation *op) {
    if (op == function.getOperation())
      return;
    if (op->getNumResults() == 1)
      if (auto name = op->getAttrOfType<StringAttr>("pyc.name")) {
        auto it = valueIds.find(op->getResult(0));
        if (it == valueIds.end() || it->second >= values.size() ||
            values[it->second].probeName != name.getValue())
          missingNamedValue = true;
        else
          expectedNamedProbes.push_back(it->second);
      }
    if (fineIndex >= fineNodes.size() || fineNodes[fineIndex].op != op)
      fineMismatch = true;
    ++fineIndex;
  });
  if (missingNamedValue || expectedNamedProbes != namedProbeValueIds)
    return function.emitError("simulation named probe IDs are stale");
  if (fineMismatch || fineIndex != fineNodes.size())
    return function.emitError("simulation graph fine nodes do not cover the operation tree");
  DerivedConnectivity derived = deriveConnectivity(*this);
  if (hasAmbiguousDrivers != derived.hasAmbiguousDrivers ||
      edges.size() != derived.edges.size())
    return function.emitError("simulation graph dependency set is incomplete");
  for (auto [edge, expected] : llvm::zip(edges, derived.edges))
    if (edge.producer != expected.producer || edge.consumer != expected.consumer ||
        edge.valueId != expected.valueId ||
        edge.value != expected.value ||
        edge.crossesStateBoundary != expected.crossesStateBoundary ||
        edge.reachesObservable != expected.reachesObservable)
      return function.emitError("simulation graph dependency set is stale");
  return success();
}

} // namespace pyc
