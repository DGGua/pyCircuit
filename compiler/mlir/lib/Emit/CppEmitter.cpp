#include "pyc/Emit/CppEmitter.h"


#include "mlir/Dialect/Arith/IR/Arith.h"
#include "mlir/Dialect/Func/IR/FuncOps.h"
#include "mlir/IR/BuiltinOps.h"
#include "mlir/IR/Types.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/ADT/StringSet.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/JSON.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>
#include <cstdint>
#include <functional>
#include <set>
#include <vector>

using namespace mlir;

namespace pyc {
namespace {

static std::string sanitizeId(llvm::StringRef s) {
  std::string out;
  out.reserve(s.size() + 1);
  auto isAlpha = [](char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); };
  auto isDigit = [](char c) { return (c >= '0' && c <= '9'); };
  auto isOk = [&](char c) { return isAlpha(c) || isDigit(c) || c == '_'; };

  for (char c : s) {
    out.push_back(isOk(c) ? c : '_');
  }
  if (out.empty() || isDigit(out.front()))
    out.insert(out.begin(), '_');
  return out;
}

static std::string cppStringLiteral(llvm::StringRef s) {
  std::string out;
  out.reserve(s.size() + 2);
  out.push_back('"');
  for (char c : s) {
    switch (c) {
    case '\\':
      out += "\\\\\\\\";
      break;
    case '"':
      out += "\\\\\"";
      break;
    case '\n':
      out += "\\\\n";
      break;
    case '\r':
      out += "\\\\r";
      break;
    case '\t':
      out += "\\\\t";
      break;
    default:
      out.push_back(c);
      break;
    }
  }
  out.push_back('"');
  return out;
}

static std::string cppType(const SimType &type) {
  std::string name = "pyc::cpp::Wire<" + std::to_string(type.width) + ">";
  for (auto it = type.shape.rbegin(); it != type.shape.rend(); ++it)
    name = "pyc::cpp::Vec<" + name + ", " + std::to_string(*it) + ">";
  return name;
}

// Vectors register recursively through ProbeRegistry::addVec.
static void emitWireProbes(llvm::raw_ostream &os, const SimType &type,
                           const std::string &fieldPath,
                           const std::string &cppExpr) {
  if (!type.shape.empty()) {
    os << "    reg.addVec(reg_path(" << cppStringLiteral(fieldPath) << "), &" << cppExpr << ");\n";
    return;
  }
  os << "    reg.addWire<" << type.width << ">(reg_path(" << cppStringLiteral(fieldPath) << "), &" << cppExpr << ");\n";
}

struct NameTable {
  const SimGraph &graph;
  llvm::DenseMap<unsigned, std::string> names;
  llvm::StringMap<unsigned> used;
  int next = 0;

  explicit NameTable(const SimGraph &graph) : graph(graph) {}

  std::string unique(std::string base) {
    unsigned &n = used[base];
    n++;
    if (n == 1)
      return base;
    return base + "_" + std::to_string(n);
  }

  std::string get(unsigned id) {
    if (auto it = names.find(id); it != names.end())
      return it->second;
    const SimValue &value = graph.values[id];
    if (value.sourceNameIsExplicit) {
      std::string cand = unique(sanitizeId(value.sourceNameBase));
      names.try_emplace(id, cand);
      return cand;
    }
    if (!value.sourceNameBase.empty()) {
      // Fall back to op-based names for readability (instead of v1/v2/...).
      std::string base = sanitizeId(value.sourceNameBase);
      if (base.empty())
        base = "v";
      base += "_" + std::to_string(++next);
      std::string cand = unique(base);
      names.try_emplace(id, cand);
      return cand;
    }
    std::string n = unique("arg_" + std::to_string(++next));
    names.try_emplace(id, n);
    return n;
  }

};

static std::string treeReduceExpr(llvm::SmallVectorImpl<std::string> &terms,
                                  llvm::StringRef op) {
  while (terms.size() > 1) {
    llvm::SmallVector<std::string> next;
    for (size_t i = 0; i < terms.size(); i += 2) {
      if (i + 1 < terms.size())
        next.push_back("(" + terms[i] + " " + op.str() + " " + terms[i + 1] + ")");
      else
        next.push_back(terms[i]);
    }
    terms = std::move(next);
  }
  return terms.empty() ? "" : terms[0];
}

static std::string chainReduceExpr(llvm::SmallVectorImpl<std::string> &terms,
                                   llvm::StringRef op) {
  if (terms.empty())
    return "";
  std::string out = terms[0];
  for (size_t i = 1; i < terms.size(); ++i)
    out = "(" + out + " " + op.str() + " " + terms[i] + ")";
  return out;
}

struct ProbeAliasEntry {
  std::string canonicalPath;
  std::string sourcePath;
};

static std::vector<ProbeAliasEntry> loadProbeAliasesForTop(llvm::StringRef planPath, llvm::StringRef symName) {
  std::vector<ProbeAliasEntry> out;
  if (planPath.empty())
    return out;
  auto fileOrErr = llvm::MemoryBuffer::getFile(planPath);
  if (!fileOrErr)
    return out;
  auto parsed = llvm::json::parse(fileOrErr.get()->getBuffer());
  if (!parsed)
    return out;
  auto *obj = parsed->getAsObject();
  if (!obj)
    return out;
  auto topSymbol = obj->getString("top_symbol");
  if (!topSymbol || *topSymbol != symName)
    return out;
  auto *aliases = obj->getArray("aliases");
  if (!aliases)
    return out;
  out.reserve(aliases->size());
  for (const llvm::json::Value &value : *aliases) {
    auto *entry = value.getAsObject();
    if (!entry)
      continue;
    auto canonical = entry->getString("canonical_path");
    auto source = entry->getString("source_path");
    if (!canonical || !source)
      continue;
    out.push_back(ProbeAliasEntry{canonical->str(), source->str()});
  }
  std::sort(out.begin(), out.end(), [](const ProbeAliasEntry &a, const ProbeAliasEntry &b) {
    return a.canonicalPath < b.canonicalPath;
  });
  return out;
}

// Emit a copied expression description. The original MLIR operation is not
// consulted for its opcode, operands, widths, constants or attributes here.
static LogicalResult emitGraphExpr(const SimGraph &graph, const SimExpr &expr,
                                   llvm::raw_ostream &os, NameTable &nt,
                                   llvm::DenseMap<unsigned, std::string> *inlineRhs = nullptr) {
  if (expr.omitOriginalEvaluation)
    return success();
  auto type = [&](unsigned id) -> const SimType & { return graph.values[id].type; };
  auto name = [&](unsigned id) -> std::string {
    return graph.values[id].sourceBacked ? nt.get(id)
                                   : "_pyc_expr_" + std::to_string(id);
  };
  auto input = [&](unsigned index) {
    unsigned id = expr.operands[index];
    if (inlineRhs)
      if (auto it = inlineRhs->find(id); it != inlineRhs->end())
        return "(" + it->second + ")";
    return name(id);
  };
  const SimType &resultType = type(expr.result);
  unsigned width = resultType.width;
  auto inputWidth = [&](unsigned index) { return type(expr.operands[index]).width; };
  auto binary = [&](llvm::StringRef token) {
    return "(" + input(0) + " " + token.str() + " " + input(1) + ")";
  };
  auto call = [&](llvm::StringRef function, unsigned callWidth,
                  unsigned count) {
    std::string code = "pyc::cpp::" + function.str() + "<" +
                       std::to_string(callWidth) + ">(";
    for (unsigned i = 0; i < count; ++i) {
      if (i) code += ", ";
      code += input(i);
    }
    return code + ")";
  };
  std::string code;
  switch (expr.kind) {
  case SimExprKind::Constant: {
    code = cppType(resultType) + "({";
    for (unsigned i = 0; i < (width + 63u) / 64u; ++i) {
      if (i) code += ", ";
      code += "0x" + llvm::utohexstr(expr.constant.getRawData()[i]) + "ull";
    }
    code += "})";
    break;
  }
  case SimExprKind::Alias:
  case SimExprKind::ResetActive:
    code = input(0);
    break;
  case SimExprKind::Add: code = binary("+"); break;
  case SimExprKind::Sub: code = binary("-"); break;
  case SimExprKind::Mul: code = binary("*"); break;
  case SimExprKind::And: code = binary("&"); break;
  case SimExprKind::Or: code = binary("|"); break;
  case SimExprKind::Xor: code = binary("^"); break;
  case SimExprKind::Not: code = "(~" + input(0) + ")"; break;
  case SimExprKind::Udiv: code = call("udiv", width, 2); break;
  case SimExprKind::Urem: code = call("urem", width, 2); break;
  case SimExprKind::Sdiv: code = call("sdiv", width, 2); break;
  case SimExprKind::Srem: code = call("srem", width, 2); break;
  case SimExprKind::Eq: code = call("eq", inputWidth(0), 2); break;
  case SimExprKind::Ult: code = call("ult", inputWidth(0), 2); break;
  case SimExprKind::Slt: code = call("slt", inputWidth(0), 2); break;
  case SimExprKind::Mux:
    code = call(type(expr.operands[0]).shape.empty() ? "mux" : "mux_vec",
                width, 3);
    break;
  case SimExprKind::Select:
    code = "(" + input(0) + ".toBool() ? " + input(1) + " : " + input(2) + ")";
    break;
  case SimExprKind::Trunc:
  case SimExprKind::Zext:
  case SimExprKind::Sext:
  case SimExprKind::Extract: {
    llvm::StringRef function = expr.kind == SimExprKind::Trunc ? "trunc" :
                               expr.kind == SimExprKind::Zext ? "zext" :
                               expr.kind == SimExprKind::Sext ? "sext" : "extract";
    code = "pyc::cpp::" + function.str();
    if (!type(expr.operands[0]).shape.empty()) code += "_vec";
    code += "<" + std::to_string(width) + ", " +
            std::to_string(inputWidth(0)) + ">(" + input(0);
    if (expr.kind == SimExprKind::Extract)
      code += ", " + std::to_string(expr.immediate) + "u";
    code += ")";
    break;
  }
  case SimExprKind::Shli:
  case SimExprKind::Lshri:
  case SimExprKind::Ashri:
  case SimExprKind::Shl:
  case SimExprKind::Lshr:
  case SimExprKind::Ashr: {
    llvm::StringRef function =
        (expr.kind == SimExprKind::Shli || expr.kind == SimExprKind::Shl) ? "shl" :
        (expr.kind == SimExprKind::Lshri || expr.kind == SimExprKind::Lshr) ? "lshr" : "ashr";
    bool variable = expr.kind == SimExprKind::Shl ||
                    expr.kind == SimExprKind::Lshr ||
                    expr.kind == SimExprKind::Ashr;
    code = "pyc::cpp::" + function.str() + "<" + std::to_string(width) +
           ">(" + input(0) + ", " +
           (variable ? "static_cast<unsigned>(" + input(1) + ".value())"
                     : std::to_string(expr.immediate) + "u") + ")";
    break;
  }
  case SimExprKind::Concat:
    code = "pyc::cpp::concat(";
    for (unsigned i = 0; i < expr.operands.size(); ++i) {
      if (i) code += ", ";
      code += input(i);
    }
    code += ")";
    break;
  case SimExprKind::VGet:
    code = input(0) + "[" + std::to_string(expr.immediate) + "]";
    break;
  case SimExprKind::VCreate:
    code = cppType(resultType) + "{{";
    for (unsigned i = 0; i < expr.operands.size(); ++i) {
      if (i) code += ", ";
      code += input(i);
    }
    code += "}}";
    break;
  case SimExprKind::VBroadcast:
    code = cppType(resultType) + "{{";
    for (int64_t i = 0; i < expr.immediate; ++i) {
      if (i) code += ", ";
      code += input(0);
    }
    code += "}}";
    break;
  case SimExprKind::VBroadcastDim: {
    const auto &shape = resultType.shape;
    if (expr.dimension < 0 ||
        static_cast<size_t>(expr.dimension) >= shape.size())
      return failure();
    uint64_t count = 1;
    for (int64_t extent : shape) {
      if (extent <= 0)
        return failure();
      count *= static_cast<uint64_t>(extent);
    }
    code = cppType(resultType) + "{{";
    llvm::SmallVector<int64_t> indices(shape.size(), 0);
    for (uint64_t lane = 0; lane < count; ++lane) {
      if (lane)
        code += ", ";
      code += input(0);
      for (size_t axis = 0; axis < shape.size(); ++axis)
        if (static_cast<int64_t>(axis) != expr.dimension)
          code += "[" + std::to_string(indices[axis]) + "]";
      for (size_t axis = shape.size(); axis-- > 0;) {
        if (++indices[axis] < shape[axis])
          break;
        indices[axis] = 0;
      }
    }
    code += "}}";
    break;
  }
  case SimExprKind::VOrReduce:
  case SimExprKind::VAndReduce:
  case SimExprKind::VAddReduce: {
    llvm::StringRef token = expr.kind == SimExprKind::VOrReduce ? "|" :
                            expr.kind == SimExprKind::VAndReduce ? "&" : "+";
    const auto &shape = type(expr.operands[0]).shape;
    auto reduce = [&](llvm::SmallVector<std::string> &terms) {
      return expr.treeReduce ? treeReduceExpr(terms, token)
                             : chainReduceExpr(terms, token);
    };
    if (expr.dimension < 0 || shape.size() == 1) {
      llvm::SmallVector<std::string> terms;
      if (shape.size() == 1) {
        for (int64_t i = 0; i < shape[0]; ++i)
          terms.push_back(input(0) + "[" + std::to_string(i) + "]");
      } else if (shape.size() == 2) {
        for (int64_t i = 0; i < shape[0]; ++i)
          for (int64_t j = 0; j < shape[1]; ++j)
            terms.push_back(input(0) + "[" + std::to_string(i) + "][" +
                            std::to_string(j) + "]");
      }
      code = reduce(terms);
    } else if (shape.size() == 2) {
      code = cppType(resultType) + "{{";
      int64_t outputs = expr.dimension == 0 ? shape[1] : shape[0];
      int64_t lanes = expr.dimension == 0 ? shape[0] : shape[1];
      for (int64_t i = 0; i < outputs; ++i) {
        if (i) code += ", ";
        llvm::SmallVector<std::string> terms;
        for (int64_t j = 0; j < lanes; ++j) {
          int64_t row = expr.dimension == 0 ? j : i;
          int64_t col = expr.dimension == 0 ? i : j;
          terms.push_back(input(0) + "[" + std::to_string(row) + "][" +
                          std::to_string(col) + "]");
        }
        code += reduce(terms);
      }
      code += "}}";
    }
    break;
  }
  }
  if (code.empty())
    return failure();
  if (expr.inlineIntoConsumer && inlineRhs) {
    inlineRhs->try_emplace(expr.result, std::move(code));
    return success();
  }
  os << "    " << name(expr.result) << " = " << code << ";\n";
  return success();
}

static LogicalResult emitCombMethod(const SimGraph &graph,
                                    const SimCombRegion &region,
                                    const CombRegionStatementPlan &statements,
                                    const llvm::DenseMap<unsigned,
                                        CombRegionStatementPlan> &regionPlans,
                                    llvm::raw_ostream &os,
                                    NameTable &nt,
                                    unsigned idx) {
  if (region.inputIds.size() != region.argumentIds.size() ||
      region.resultIds.size() != region.yieldIds.size())
    return region.source->emitError("simulation comb region is incomplete");
  // The graph records block argument bindings and step order during IR import.
  for (auto [argId, inputId] : llvm::zip(region.argumentIds,
                                          region.inputIds))
    nt.names.try_emplace(argId,
                         nt.get(inputId));
  std::function<LogicalResult(unsigned)> emitNested;
  emitNested = [&](unsigned nestedId) -> LogicalResult {
    const SimCombRegion &nested = graph.combRegions[nestedId];
    for (auto [argId, inputId] : llvm::zip(nested.argumentIds,
                                            nested.inputIds))
      nt.names.try_emplace(argId,
                           nt.get(inputId));
    for (const auto &chunk : regionPlans.find(nestedId)->second.chunks)
      for (const SimCombStep &step : chunk) {
        if (step.expressionId != ~0u) {
          if (failed(emitGraphExpr(graph, graph.expressions[step.expressionId],
                                   os, nt)))
            return failure();
        } else if (failed(emitNested(step.nestedRegionId))) {
          return failure();
        }
      }
    for (auto [resultId, yieldId] : llvm::zip(nested.resultIds,
                                               nested.yieldIds))
      os << "    " << nt.get(resultId) << " = "
         << nt.get(yieldId) << ";\n";
    return success();
  };
  auto emitStep = [&](const SimCombStep &step) -> LogicalResult {
    if (step.expressionId != ~0u)
      return emitGraphExpr(graph, graph.expressions[step.expressionId],
                           os, nt);
    return emitNested(step.nestedRegionId);
  };
  auto emitYields = [&]() {
    for (auto [resultId, yieldId] : llvm::zip(region.resultIds,
                                              region.yieldIds))
      os << "    " << nt.get(resultId) << " = "
         << nt.get(yieldId) << ";\n";
  };

  if (statements.chunks.size() > 1) {
    std::vector<std::string> partMethods;
    partMethods.reserve(statements.chunks.size());
    for (auto [partIdx, chunk] : llvm::enumerate(statements.chunks)) {
      std::string partName = "eval_comb_" + std::to_string(idx) + "_part_" + std::to_string(partIdx);
      partMethods.push_back(partName);
      os << "  inline void " << partName << "() {\n";
      for (const SimCombStep &step : chunk)
        if (failed(emitStep(step)))
          return failure();
      os << "  }\n\n";
    }

    os << "  inline void eval_comb_" << idx << "() {\n";
    for (const std::string &partName : partMethods)
      os << "    " << partName << "();\n";

    emitYields();
    os << "  }\n\n";
    return success();
  }

  os << "  inline void eval_comb_" << idx << "() {\n";
  for (const SimCombStep &step : statements.chunks.front())
    if (failed(emitStep(step)))
      return failure();
  emitYields();
  os << "  }\n\n";
  return success();
}

static LogicalResult emitSimGroupMethod(const SimulationPlan &plan,
                                        unsigned groupNodeId,
                                        llvm::raw_ostream &os,
                                        NameTable &nt, unsigned index,
                                        const GroupActivationPlan *activation) {
  const SimGraph &graph = plan.graph;
  const GroupStatementPlan &statements =
      plan.groupStatements.find(groupNodeId)->second;
  std::string method = "eval_sim_group_" + std::to_string(index);
  llvm::DenseMap<unsigned, std::string> inlineRhs;
  for (unsigned id : statements.replicatedExpressionIds)
    if (failed(emitGraphExpr(graph, graph.expressions[id], os, nt, &inlineRhs)))
      return failure();
  auto emitStatementChunk =
      [&](const llvm::SmallVector<SimStatement> &chunk) -> LogicalResult {
    for (const SimStatement &statement : chunk) {
      if (statement.kind == SimStatementKind::Expression) {
        if (failed(emitGraphExpr(graph,
                                 graph.expressions[statement.expressionId],
                                 os, nt, &inlineRhs)))
          return failure();
        continue;
      }
      os << "    if (" << nt.get(statement.selectorId)
         << ".toBool()) {\n";
      for (const PlannedMuxAssignment &assignment :
           statement.muxAssignments) {
        os << "      " << nt.get(assignment.resultId)
           << " = " << nt.get(assignment.trueValueId)
           << ";\n";
      }
      os << "    } else {\n";
      for (const PlannedMuxAssignment &assignment :
           statement.muxAssignments) {
        os << "      " << nt.get(assignment.resultId)
           << " = " << nt.get(assignment.falseValueId)
           << ";\n";
      }
      os << "    }\n";
    }
    return success();
  };
  auto emitActivationCheck = [&]() {
    if (!activation)
      return;
    std::string prefix = "_pyc_group_" + std::to_string(index);
    auto emitInput = [&](unsigned i, unsigned inputId) {
      const SimValue &input = graph.values[inputId];
      if (i >= activation->usedBits.size() ||
          activation->usedBits[i].isAllOnes()) {
        os << nt.get(inputId);
        return;
      }
      const llvm::APInt &mask = activation->usedBits[i];
      unsigned width = input.type.width;
      os << "(" << nt.get(inputId) << " & pyc::cpp::Wire<" << width << ">({";
      for (unsigned word = 0; word < (width + 63u) / 64u; ++word) {
        if (word)
          os << ", ";
        os << mask.extractBitsAsZExtValue(
                  std::min(64u, width - word * 64u), word * 64u)
           << "ull";
      }
      os << "}))";
    };
    os << "    if (" << prefix << "_valid";
    for (auto [i, input] : llvm::enumerate(activation->inputIds)) {
      const SimType &type = graph.values[input].type;
      bool selectedElements = type.shape.size() == 2 &&
                              i < activation->vectorElements.size() &&
                              !activation->vectorElements[i].isAllOnes();
      bool selectedLanes = !type.shape.empty() &&
                           i < activation->vectorLanes.size() &&
                           !activation->vectorLanes[i].isAllOnes();
      if (selectedElements) {
        const llvm::APInt &mask = activation->vectorElements[i];
        unsigned columns = static_cast<unsigned>(type.shape[1]);
        for (unsigned element = 0; element < mask.getBitWidth(); ++element)
          if (mask[element]) {
            unsigned row = element / columns, col = element % columns;
            os << " && " << prefix << "_in_" << i << "[" << row << "]["
               << col << "] == " << nt.get(input) << "[" << row << "]["
               << col << "]";
          }
      } else if (selectedLanes) {
        const llvm::APInt &mask = activation->vectorLanes[i];
        for (unsigned lane = 0; lane < mask.getBitWidth(); ++lane)
          if (mask[lane])
            os << " && " << prefix << "_in_" << i << "[" << lane
               << "] == " << nt.get(input) << "[" << lane << "]";
      } else {
        os << " && " << prefix << "_in_" << i << " == ";
        emitInput(i, input);
      }
    }
    os << ") {\n";
    os << "      if (_pyc_sim_stats_enable) _pyc_sim_stats.group_cache_skips++;\n";
    os << "      return;\n";
    os << "    }\n";
    for (auto [i, input] : llvm::enumerate(activation->inputIds)) {
      const SimType &type = graph.values[input].type;
      std::string cached = prefix + "_in_" + std::to_string(i);
      if (type.shape.size() == 2 &&
          i < activation->vectorElements.size() &&
          !activation->vectorElements[i].isAllOnes()) {
        const llvm::APInt &elements = activation->vectorElements[i];
        unsigned columns = static_cast<unsigned>(type.shape[1]);
        for (unsigned element = 0; element < elements.getBitWidth(); ++element)
          if (elements[element]) {
            unsigned row = element / columns, col = element % columns;
            os << "    " << cached << "[" << row << "][" << col << "] = "
               << nt.get(input) << "[" << row << "][" << col << "];\n";
          }
        continue;
      }
      if (!type.shape.empty() && i < activation->vectorLanes.size() &&
          !activation->vectorLanes[i].isAllOnes()) {
        const llvm::APInt &lanes = activation->vectorLanes[i];
        for (unsigned lane = 0; lane < lanes.getBitWidth(); ++lane)
          if (lanes[lane])
            os << "    " << cached << "[" << lane << "] = "
               << nt.get(input) << "[" << lane << "];\n";
        continue;
      }
      os << "    " << prefix << "_in_" << i << " = ";
      emitInput(i, input);
      os << ";\n";
    }
    os << "    " << prefix << "_valid = true;\n";
    os << "    if (_pyc_sim_stats_enable) _pyc_sim_stats.group_eval_calls++;\n";
  };
  std::set<unsigned> trackedOutputs;
  if (auto it = plan.groupPropagations.find(groupNodeId);
      it != plan.groupPropagations.end())
    for (const GroupPropagationTarget &target : it->second)
      trackedOutputs.insert(target.valueIds.begin(), target.valueIds.end());
  auto oldOutputName = [](unsigned id) {
    return "_pyc_old_group_value_" + std::to_string(id);
  };
  auto emitOutputSnapshots = [&]() {
    for (unsigned value : trackedOutputs)
      os << "    auto " << oldOutputName(value) << " = "
         << nt.get(value) << ";\n";
  };
  auto emitPropagation = [&]() {
    auto it = plan.groupPropagations.find(groupNodeId);
    if (it == plan.groupPropagations.end())
      return;
    for (const GroupPropagationTarget &target : it->second) {
      unsigned targetIndex = plan.groupByNode.lookup(target.groupNodeId);
      os << "    if (";
      for (auto [i, value] : llvm::enumerate(target.valueIds)) {
        if (i)
          os << " || ";
        const SimType &type = graph.values[value].type;
        if (type.shape.size() == 2 &&
            !target.vectorElements[i].isAllOnes()) {
          const llvm::APInt &elements = target.vectorElements[i];
          unsigned columns = static_cast<unsigned>(type.shape[1]);
          os << "(";
          bool firstElement = true;
          for (unsigned element = 0; element < elements.getBitWidth(); ++element) {
            if (!elements[element])
              continue;
            if (!firstElement)
              os << " || ";
            firstElement = false;
            unsigned row = element / columns, col = element % columns;
            os << nt.get(value) << "[" << row << "][" << col << "] != "
               << oldOutputName(value) << "[" << row << "][" << col << "]";
          }
          if (firstElement)
            os << "false";
          os << ")";
          continue;
        }
        if (!type.shape.empty() &&
            !target.vectorLanes[i].isAllOnes()) {
          const llvm::APInt &lanes = target.vectorLanes[i];
          os << "(";
          bool firstLane = true;
          for (unsigned lane = 0; lane < lanes.getBitWidth(); ++lane) {
            if (!lanes[lane])
              continue;
            if (!firstLane)
              os << " || ";
            firstLane = false;
            os << nt.get(value) << "[" << lane << "] != "
               << oldOutputName(value) << "[" << lane << "]";
          }
          if (firstLane)
            os << "false";
          os << ")";
          continue;
        }
        const llvm::APInt &mask = target.usedBits[i];
        auto emitMaskedValue = [&](llvm::StringRef name) {
          if (mask.isAllOnes()) {
            os << name;
            return;
          }
          unsigned width = graph.values[value].type.width;
          os << "(" << name << " & pyc::cpp::Wire<" << width << ">({";
          for (unsigned word = 0; word < (width + 63u) / 64u; ++word) {
            if (word)
              os << ", ";
            os << mask.extractBitsAsZExtValue(
                      std::min(64u, width - word * 64u), word * 64u)
               << "ull";
          }
          os << "}))";
        };
        emitMaskedValue(nt.get(value));
        os << " != ";
        emitMaskedValue(oldOutputName(value));
      }
      os << ") _pyc_group_active_flags[" << targetIndex / 64u
         << "] |= (1ull << " << targetIndex % 64u << ");\n";
    }
  };
  if (statements.chunks.size() > 1) {
    for (size_t part = 0; part < statements.chunks.size(); ++part) {
      os << "  inline void " << method << "_part_" << part << "() {\n";
      if (failed(emitStatementChunk(statements.chunks[part])))
        return failure();
      os << "  }\n\n";
    }
    os << "  inline void " << method << "() {\n";
    emitActivationCheck();
    emitOutputSnapshots();
    for (size_t part = 0; part < statements.chunks.size(); ++part)
      os << "    " << method << "_part_" << part << "();\n";
    emitPropagation();
    os << "  }\n\n";
    return success();
  }
  os << "  inline void " << method << "() {\n";
  emitActivationCheck();
  emitOutputSnapshots();
  if (failed(emitStatementChunk(statements.chunks.front())))
    return failure();
  emitPropagation();
  os << "  }\n\n";
  return success();
}

static LogicalResult emitFunc(const SimulationPlan &plan, llvm::raw_ostream &os, const CppEmitterOptions &opts) {
  func::FuncOp f = plan.graph.function;
  if (failed(plan.verify()))
    return f.emitError("C++ emitter received an incomplete simulation plan");
  NameTable nt(plan.graph);

  std::string structName = sanitizeId(plan.graph.functionName);
  os << "struct " << structName << " {\n";

  // Ports.
  std::vector<std::string> inNames;
  inNames.reserve(plan.graph.inputValueIds.size());
  std::vector<std::string> inCanon;
  inCanon.reserve(plan.graph.inputValueIds.size());
  std::vector<std::string> outNames;
  outNames.reserve(plan.graph.outputValueIds.size());
  std::vector<std::string> outCanon;
  outCanon.reserve(plan.graph.outputValueIds.size());
  for (auto [i, valueId] : llvm::enumerate(plan.graph.inputValueIds)) {
    const SimValue &arg = plan.graph.values[valueId];
    inCanon.push_back(plan.graph.inputPortPaths[i]);
    std::string name = nt.unique(sanitizeId(inCanon.back()));
    inNames.push_back(name);
    nt.names.try_emplace(valueId, name);
    os << "  " << cppType(arg.type) << " " << name << "{};\n";
  }
  for (unsigned i = 0; i < plan.graph.outputValueIds.size(); ++i) {
    outCanon.push_back(plan.graph.outputPortPaths[i]);
    std::string name = nt.unique(sanitizeId(outCanon.back()));
    outNames.push_back(name);
    os << "  " << cppType(plan.graph.values[plan.graph.outputValueIds[i]].type)
       << " " << name << "{};\n";
  }
  os << "\n";

  // Internal wires for op results (including inside pyc.comb regions).
  struct Decl {
    std::string name;
    SimType type;
  };
  std::vector<Decl> decls;
  decls.reserve(plan.graph.declarationValueIds.size());
  for (unsigned id : plan.graph.declarationValueIds) {
    const SimValue &value = plan.graph.values[id];
    decls.push_back(Decl{nt.get(id), value.type});
  }
  std::sort(decls.begin(), decls.end(), [](const Decl &a, const Decl &b) { return a.name < b.name; });
  for (const Decl &d : decls)
    os << "  " << cppType(d.type) << " " << d.name << "{};\n";
  os << "\n";

  for (auto [i, nodeIndex] : llvm::enumerate(plan.graph.groupedNodes)) {
    auto activation = plan.groupActivations.find(nodeIndex);
    if (activation == plan.groupActivations.end())
      continue;
    std::string prefix = "_pyc_group_" + std::to_string(i);
    os << "  bool " << prefix << "_valid = false;\n";
    for (auto [inputIndex, inputId] : llvm::enumerate(activation->second.inputIds))
      os << "  " << cppType(plan.graph.values[inputId].type) << " " << prefix << "_in_"
         << inputIndex << "{};\n";
  }
  if (!plan.packedActivationGroups.empty()) {
    unsigned words = (plan.graph.groupedNodes.size() + 63u) / 64u;
    os << "  std::uint64_t _pyc_group_active_flags[" << words << "] = {";
    for (unsigned word = 0; word < words; ++word) {
      if (word)
        os << ", ";
      os << "~0ull";
    }
    os << "};\n";
  }
  for (auto [i, group] : llvm::enumerate(plan.resetGroups)) {
    (void)group;
    os << "  bool _pyc_reset_group_clk_prev_" << i << " = false;\n";
  }

  // The simulation plan fixes primitive, instance and comb order.
  llvm::SmallVector<unsigned> regs = plan.operationOrder.regs;
  llvm::SmallVector<unsigned> fifos = plan.operationOrder.fifos;
  llvm::SmallVector<unsigned> byteMems = plan.operationOrder.byteMems;
  llvm::SmallVector<unsigned> syncMems = plan.operationOrder.syncMems;
  llvm::SmallVector<unsigned> syncMemDPs = plan.operationOrder.syncMemDPs;
  llvm::SmallVector<unsigned> asyncFifos = plan.operationOrder.asyncFifos;
  llvm::SmallVector<unsigned> cdcSyncs = plan.operationOrder.cdcSyncs;
  llvm::SmallVector<unsigned> instances = plan.operationOrder.instances;
  auto graphValueName = [&](unsigned valueId) {
    return nt.get(valueId);
  };
  auto fifoInputName = [&](unsigned nodeId, unsigned index) {
    return graphValueName(plan.graph.topNodes[nodeId].inputIds[index]);
  };
  auto fifoOutputName = [&](unsigned nodeId, unsigned index) {
    return graphValueName(plan.graph.topNodes[nodeId].outputIds[index]);
  };
  auto fifoInstanceName = [&](unsigned nodeId) {
    return fifoOutputName(nodeId, 0) + "_inst";
  };
  auto memoryInstanceName = [&](unsigned nodeId) {
    const SimNode &mem = plan.graph.topNodes[nodeId];
    return mem.primitiveHasName ? sanitizeId(mem.primitiveName)
                                : graphValueName(mem.outputIds.front()) + "_inst";
  };

  struct InstInfo {
    unsigned nodeId;
    std::string calleeName;
    std::string member;
    std::string seg;
    std::vector<std::string> inPorts;
    std::vector<std::string> outPorts;
  };
  std::vector<InstInfo> instInfos;
  instInfos.reserve(instances.size());
  llvm::DenseMap<unsigned, unsigned> instIndex;
  if (!instances.empty()) {
    for (unsigned nodeId : instances) {
      const SimNode &inst = plan.graph.topNodes[nodeId];
      const InstanceInterfacePlan &interface = plan.instanceInterfaces.find(nodeId)->second;
      std::vector<std::string> inPorts = interface.inputPorts;
      std::vector<std::string> outPorts = interface.outputPorts;

      std::string base = "inst";
      if (inst.instanceHasName)
        base = sanitizeId(inst.instanceName);
      else
        base = sanitizeId(inst.instanceCallee) + std::string("_inst");
      std::string seg = base;
      if (inst.instanceHasShortName)
        seg = sanitizeId(inst.instanceShortName);
      std::string member = nt.unique(base);

      unsigned idx = static_cast<unsigned>(instInfos.size());
      instIndex.try_emplace(nodeId, idx);
      instInfos.push_back(InstInfo{nodeId, inst.instanceCallee, std::move(member), std::move(seg), std::move(inPorts), std::move(outPorts)});
    }

  }

  auto instancePackedCacheWordCount = [&](const InstInfo &ii) -> unsigned {
    return plan.instanceCaches.lookup(ii.nodeId).packedWords;
  };
  auto usePackedInstanceEvalCache = [&](const InstInfo &ii) -> bool {
    return plan.instanceCaches.lookup(ii.nodeId).usePackedWords;
  };


	  if (!instInfos.empty()) {
	    os << "  // Sub-modules.\n";
	    for (const auto &ii : instInfos) {
	      // Decision 0012: Parent SimObjects own children via unique_ptr.
	      os << "  std::unique_ptr<" << sanitizeId(ii.calleeName) << "> " << ii.member
	         << " = std::make_unique<" << sanitizeId(ii.calleeName) << ">();\n";
	    }
	    os << "\n";

    os << "  // Sub-module eval cache (default-on in C++; can be disabled with\n";
    os << "  // -DPYC_DISABLE_INSTANCE_EVAL_CACHE).\n";
    for (const auto &ii : instInfos) {
      const SimNode &inst = plan.graph.topNodes[ii.nodeId];
      os << "  bool " << ii.member << "_eval_cache_valid = false;\n";
      if (usePackedInstanceEvalCache(ii)) {
        os << "  std::array<std::uint64_t, " << instancePackedCacheWordCount(ii) << "> " << ii.member
           << "_eval_cache_words{};\n";
      } else {
        for (unsigned i = 0; i < inst.inputIds.size(); ++i) {
          std::string cacheName = ii.member + "_eval_cache_in_" + std::to_string(i);
          os << "  " << cppType(plan.graph.values[inst.inputIds[i]].type) << " " << cacheName << "{};\n";
          os << "  std::uint64_t " << ii.member << "_eval_cache_in_ver_" << i << " = 1ull;\n";
          os << "  std::uint64_t " << ii.member << "_eval_cache_in_seen_ver_" << i << " = 0ull;\n";
          if (plan.fingerprintInputs.contains(inst.inputIds[i]))
            os << "  std::uint64_t " << ii.member << "_eval_cache_in_fp_" << i << " = 0ull;\n";
        }
      }
    }
    os << "\n";
  }

	  // DFX trace registration (Decision 0145).
	  os << "  template <typename TbT, typename EnabledInstT, typename EnabledSigT>\n";
	  os << "  void pyc_trace_vcd(TbT &tb, const std::string &prefix, EnabledInstT &&enabledInst, EnabledSigT &&enabledSig) {\n";
	  os << "    std::string inst = pyc::cpp::shortenInstancePath(prefix);\n";
	  os << "    auto trace_port = [&](auto &sig, const char *leaf) {\n";
  os << "      std::string p = inst;\n";
  // Decision 0023: canonical_path uses <instance_path>:<field_path>.
  os << "      p += \":\";\n";
  os << "      p += leaf;\n";
		  os << "      if (enabledSig(p)) tb.vcdTrace(sig, p);\n";
	  os << "    };\n";
	  for (unsigned i = 0; i < inNames.size(); ++i)
	    os << "    trace_port(" << inNames[i] << ", " << cppStringLiteral(inCanon[i]) << ");\n";
	  for (unsigned i = 0; i < outNames.size(); ++i)
	    os << "    trace_port(" << outNames[i] << ", " << cppStringLiteral(outCanon[i]) << ");\n";
	  if (!instInfos.empty()) {
	    os << "    auto trace_child = [&](auto &child, const char *seg) {\n";
	    os << "      std::string full = prefix;\n";
	    os << "      full += \".\";\n";
    os << "      full += seg;\n";
    os << "      std::string inst_path = pyc::cpp::shortenInstancePath(full);\n";
    os << "      if (enabledInst(inst_path) && child) child->pyc_trace_vcd(tb, full, enabledInst, enabledSig);\n";
    os << "    };\n";
    for (const auto &ii : instInfos)
      os << "    trace_child(" << ii.member << ", \"" << ii.seg << "\");\n";
  }
	  os << "  }\n\n";

	  // ProbeRegistry registration (Decisions 0004, 0018-0021).
	  os << "  void pyc_register_probes(pyc::cpp::ProbeRegistry &reg, const std::string &prefix) {\n";
	  os << "    std::string inst = pyc::cpp::shortenInstancePath(prefix);\n";
	  os << "    auto reg_path = [&](const char *leaf) {\n";
	  os << "      std::string p = inst;\n";
	  // Decision 0023: canonical_path uses <instance_path>:<field_path>.
	  os << "      p += \":\";\n";
	  os << "      p += leaf;\n";
	  os << "      return p;\n";
	  os << "    };\n";

    struct NamedProbeInfo {
      std::string fieldPath;
      std::string cppValue;
      std::string cppRegInst;
      unsigned width = 0;
      bool isReg = false;
      SimType type;
    };

    // Decision 0003 / 0051-0052: infer probe kind for ports and named internal
    // objects. A value is considered stateful iff it directly returns the q
    // output of a local pyc.reg through captured aliases or comb passthroughs.
    std::vector<bool> outIsReg(plan.graph.outputValueIds.size(), false);
    std::vector<unsigned> outRegQ(plan.graph.outputValueIds.size(), ~0u);
    std::vector<NamedProbeInfo> namedProbes;
    {
      for (unsigned i = 0; i < plan.graph.outputValueIds.size(); ++i)
        if (auto it = plan.registerProbeTargets.find(plan.graph.outputValueIds[i]);
            it != plan.registerProbeTargets.end())
          outRegQ[i] = it->second;
      for (unsigned i = 0; i < plan.graph.outputValueIds.size(); ++i)
        outIsReg[i] = outRegQ[i] != ~0u;

      llvm::StringSet<> seenNamedFields;
      for (unsigned id : plan.graph.namedProbeValueIds) {
        const SimValue &signal = plan.graph.values[id];
        unsigned width = signal.type.width;
        if (width == 0)
          continue;
        std::string fieldPath = signal.probeName;
        if (!seenNamedFields.insert(fieldPath).second)
          continue;
        unsigned regQ = ~0u;
        if (auto it = plan.registerProbeTargets.find(id);
            it != plan.registerProbeTargets.end())
          regQ = it->second;
        namedProbes.push_back(NamedProbeInfo{
            fieldPath,
            nt.get(id),
            regQ != ~0u ? (nt.get(regQ) + "_inst") : std::string(),
            width,
            regQ != ~0u,
            signal.type,
        });
      }
      std::sort(namedProbes.begin(), namedProbes.end(), [](const NamedProbeInfo &a, const NamedProbeInfo &b) {
        return a.fieldPath < b.fieldPath;
      });
    }

		  for (auto [i, valueId] : llvm::enumerate(plan.graph.inputValueIds)) {
        const SimType &type = plan.graph.values[valueId].type;
		    unsigned w = type.width;
		    if (w == 0)
		      return f.emitError("invalid input port width for ProbeRegistry: ") << inCanon[i];
		    emitWireProbes(os, type, inCanon[static_cast<unsigned>(i)], inNames[static_cast<unsigned>(i)]);
		  }
		  for (unsigned i = 0; i < plan.graph.outputValueIds.size(); ++i) {
        const SimType &type = plan.graph.values[plan.graph.outputValueIds[i]].type;
		    unsigned w = type.width;
		    if (w == 0)
		      return f.emitError("invalid output port width for ProbeRegistry: ") << outCanon[i];
		    if (outIsReg[i] && type.shape.empty()) {
		      os << "    reg.addReg<" << w << ">(reg_path(" << cppStringLiteral(outCanon[i]) << "), &" << outNames[i]
		         << ", &" << nt.get(outRegQ[i]) << "_inst->pending, &" << nt.get(outRegQ[i]) << "_inst->qNext);\n";
		    } else {
		      emitWireProbes(os, type, outCanon[i], outNames[i]);
		    }
		  }
      for (const auto &named : namedProbes) {
        if (named.isReg && named.type.shape.empty()) {
          os << "    reg.addReg<" << named.width << ">(reg_path(" << cppStringLiteral(named.fieldPath) << "), &"
             << named.cppValue << ", &" << named.cppRegInst << "->pending, &" << named.cppRegInst << "->qNext);\n";
        } else {
          emitWireProbes(os, named.type, named.fieldPath, named.cppValue);
        }
      }
		  for (unsigned id : byteMems) {
		    std::string instName = memoryInstanceName(id);
	    os << "    reg.addMem(reg_path(\"" << instName << "\"), &" << instName << ", &" << instName
	       << ".pendingWrite, &" << instName << ".latchedAddr, &" << instName << ".latchedData, &" << instName
	       << ".latchedStrb);\n";
	  }
	  for (unsigned id : syncMems) {
	    std::string instName = memoryInstanceName(id);
	    os << "    if (" << instName << ") reg.addMem(reg_path(\"" << instName << "\"), " << instName << ", &" << instName
	       << "->pendingWrite, &" << instName << "->latchedWaddr, &" << instName << "->latchedWdata, &" << instName
	       << "->latchedWstrb);\n";
	  }
	  for (unsigned id : syncMemDPs) {
	    std::string instName = memoryInstanceName(id);
	    os << "    if (" << instName << ") reg.addMem(reg_path(\"" << instName << "\"), " << instName << ", &" << instName
	       << "->pendingWrite, &" << instName << "->latchedWaddr, &" << instName << "->latchedWdata, &" << instName
	       << "->latchedWstrb);\n";
	  }
	  if (!instInfos.empty()) {
	    os << "    auto reg_child = [&](auto &child, const char *seg) {\n";
	    os << "      std::string p = prefix;\n";
	    os << "      p += \".\";\n";
	    os << "      p += seg;\n";
	    os << "      if (child) child->pyc_register_probes(reg, p);\n";
	    os << "    };\n";
	    for (const auto &ii : instInfos)
	      os << "    reg_child(" << ii.member << ", \"" << ii.seg << "\");\n";
	  }
      auto probeAliases = loadProbeAliasesForTop(opts.probePlanPath,
                                                plan.graph.functionName);
      if (!probeAliases.empty()) {
        for (const auto &alias : probeAliases) {
          os << "    if (const auto *src = reg.findByPath(" << cppStringLiteral(alias.sourcePath) << "))\n";
          os << "      reg.addAlias(" << cppStringLiteral(alias.canonicalPath) << ", *src);\n";
        }
      }
	  os << "  }\n\n";

  for (unsigned id : regs) {
    const SimNode &reg = plan.graph.topNodes[id];
    const SimValue &q = plan.graph.values[reg.outputIds.front()];
    unsigned w = q.type.width;
    if (w == 0)
      return reg.op->emitError("invalid reg width");
    if (!q.type.shape.empty())
      os << "  pyc::cpp::pyc_vec_reg<" << cppType(q.type) << "> *" << nt.get(reg.outputIds.front()) << "_inst = nullptr;\n";
    else
      os << "  pyc::cpp::pyc_reg<" << w << "> *" << nt.get(reg.outputIds.front()) << "_inst = nullptr;\n";
  }
  for (unsigned id : fifos) {
    const SimNode &fifo = plan.graph.topNodes[id];
    unsigned w = plan.graph.values[fifo.outputIds[2]].type.width;
    if (w == 0)
      return fifo.op->emitError("invalid fifo width");
    std::string instName = fifoInstanceName(id);
    os << "  pyc::cpp::pyc_fifo<" << w << ", " << fifo.primitiveDepth
       << "> " << instName << ";\n";
    os << "  bool " << instName << "_eval_cache_valid = false;\n";
    os << "  " << cppType(plan.graph.values[fifo.inputIds[2]].type) << " " << instName << "_eval_cache_in_valid{};\n";
    os << "  std::uint64_t " << instName << "_eval_cache_in_valid_ver = 1ull;\n";
    os << "  std::uint64_t " << instName << "_eval_cache_in_valid_seen_ver = 0ull;\n";
    os << "  std::uint64_t " << instName << "_eval_cache_in_valid_fp = 0ull;\n";
    os << "  " << cppType(plan.graph.values[fifo.inputIds[3]].type) << " " << instName << "_eval_cache_in_data{};\n";
    os << "  std::uint64_t " << instName << "_eval_cache_in_data_ver = 1ull;\n";
    os << "  std::uint64_t " << instName << "_eval_cache_in_data_seen_ver = 0ull;\n";
    if (plan.fingerprintInputs.contains(fifo.inputIds[3]))
      os << "  std::uint64_t " << instName << "_eval_cache_in_data_fp = 0ull;\n";
    os << "  " << cppType(plan.graph.values[fifo.inputIds[4]].type) << " " << instName << "_eval_cache_out_ready{};\n";
    os << "  std::uint64_t " << instName << "_eval_cache_out_ready_ver = 1ull;\n";
    os << "  std::uint64_t " << instName << "_eval_cache_out_ready_seen_ver = 0ull;\n";
    os << "  std::uint64_t " << instName << "_eval_cache_out_ready_fp = 0ull;\n";
  }
  for (unsigned id : byteMems) {
    const SimNode &mem = plan.graph.topNodes[id];
    unsigned addrW = plan.graph.values[mem.inputIds[2]].type.width;
    unsigned dataW = plan.graph.values[mem.outputIds.front()].type.width;
    if (addrW == 0)
      return mem.op->emitError("invalid byte_mem addr width");
    if (dataW == 0)
      return mem.op->emitError("invalid byte_mem data width");
    std::string instName = memoryInstanceName(id);
    os << "  pyc::cpp::pyc_byte_mem<" << addrW << ", " << dataW << ", " << mem.primitiveDepth << "> " << instName << ";\n";
    os << "  bool " << instName << "_eval_cache_valid = false;\n";
    os << "  " << cppType(plan.graph.values[mem.inputIds[1]].type) << " " << instName << "_eval_cache_rst{};\n";
    os << "  std::uint64_t " << instName << "_eval_cache_rst_ver = 1ull;\n";
    os << "  std::uint64_t " << instName << "_eval_cache_rst_seen_ver = 0ull;\n";
    os << "  std::uint64_t " << instName << "_eval_cache_rst_fp = 0ull;\n";
    os << "  " << cppType(plan.graph.values[mem.inputIds[2]].type) << " " << instName << "_eval_cache_raddr{};\n";
    os << "  std::uint64_t " << instName << "_eval_cache_raddr_ver = 1ull;\n";
    os << "  std::uint64_t " << instName << "_eval_cache_raddr_seen_ver = 0ull;\n";
    if (plan.fingerprintInputs.contains(mem.inputIds[2]))
      os << "  std::uint64_t " << instName << "_eval_cache_raddr_fp = 0ull;\n";
    os << "  " << cppType(plan.graph.values[mem.inputIds[3]].type) << " " << instName << "_eval_cache_wvalid{};\n";
    os << "  std::uint64_t " << instName << "_eval_cache_wvalid_ver = 1ull;\n";
    os << "  std::uint64_t " << instName << "_eval_cache_wvalid_seen_ver = 0ull;\n";
    os << "  std::uint64_t " << instName << "_eval_cache_wvalid_fp = 0ull;\n";
    os << "  " << cppType(plan.graph.values[mem.inputIds[4]].type) << " " << instName << "_eval_cache_waddr{};\n";
    os << "  std::uint64_t " << instName << "_eval_cache_waddr_ver = 1ull;\n";
    os << "  std::uint64_t " << instName << "_eval_cache_waddr_seen_ver = 0ull;\n";
    if (plan.fingerprintInputs.contains(mem.inputIds[4]))
      os << "  std::uint64_t " << instName << "_eval_cache_waddr_fp = 0ull;\n";
    os << "  " << cppType(plan.graph.values[mem.inputIds[5]].type) << " " << instName << "_eval_cache_wdata{};\n";
    os << "  std::uint64_t " << instName << "_eval_cache_wdata_ver = 1ull;\n";
    os << "  std::uint64_t " << instName << "_eval_cache_wdata_seen_ver = 0ull;\n";
    if (plan.fingerprintInputs.contains(mem.inputIds[5]))
      os << "  std::uint64_t " << instName << "_eval_cache_wdata_fp = 0ull;\n";
    os << "  " << cppType(plan.graph.values[mem.inputIds[6]].type) << " " << instName << "_eval_cache_wstrb{};\n";
    os << "  std::uint64_t " << instName << "_eval_cache_wstrb_ver = 1ull;\n";
    os << "  std::uint64_t " << instName << "_eval_cache_wstrb_seen_ver = 0ull;\n";
    os << "  std::uint64_t " << instName << "_eval_cache_wstrb_fp = 0ull;\n";
  }
  for (unsigned id : syncMems) {
    const SimNode &mem = plan.graph.topNodes[id];
    unsigned addrW = plan.graph.values[mem.inputIds[3]].type.width;
    unsigned dataW = plan.graph.values[mem.outputIds.front()].type.width;
    if (addrW == 0)
      return mem.op->emitError("invalid sync_mem addr width");
    if (dataW == 0)
      return mem.op->emitError("invalid sync_mem data width");
    std::string instName = memoryInstanceName(id);
    os << "  pyc::cpp::pyc_sync_mem<" << addrW << ", " << dataW << ", " << mem.primitiveDepth << "> *" << instName
       << " = nullptr;\n";
  }
  for (unsigned id : syncMemDPs) {
    const SimNode &mem = plan.graph.topNodes[id];
    unsigned addrW = plan.graph.values[mem.inputIds[3]].type.width;
    unsigned dataW = plan.graph.values[mem.outputIds.front()].type.width;
    if (addrW == 0)
      return mem.op->emitError("invalid sync_mem_dp addr width");
    if (dataW == 0)
      return mem.op->emitError("invalid sync_mem_dp data width");
    std::string instName = memoryInstanceName(id);
    os << "  pyc::cpp::pyc_sync_mem_dp<" << addrW << ", " << dataW << ", " << mem.primitiveDepth << "> *" << instName
       << " = nullptr;\n";
  }
  for (unsigned id : asyncFifos) {
    const SimNode &fifo = plan.graph.topNodes[id];
    unsigned w = plan.graph.values[fifo.outputIds[2]].type.width;
    if (w == 0 || w > 64)
      return fifo.op->emitError("C++ emitter only supports async_fifo widths 1..64 in the prototype");
    std::string instName = fifoInstanceName(id);
    os << "  pyc::cpp::pyc_async_fifo<" << w << ", " << fifo.primitiveDepth
       << "> " << instName << ";\n";
    os << "  bool " << instName << "_eval_cache_valid = false;\n";
    os << "  " << cppType(plan.graph.values[fifo.inputIds[1]].type) << " " << instName << "_eval_cache_in_rst{};\n";
    os << "  std::uint64_t " << instName << "_eval_cache_in_rst_ver = 1ull;\n";
    os << "  std::uint64_t " << instName << "_eval_cache_in_rst_seen_ver = 0ull;\n";
    os << "  std::uint64_t " << instName << "_eval_cache_in_rst_fp = 0ull;\n";
    os << "  " << cppType(plan.graph.values[fifo.inputIds[3]].type) << " " << instName << "_eval_cache_out_rst{};\n";
    os << "  std::uint64_t " << instName << "_eval_cache_out_rst_ver = 1ull;\n";
    os << "  std::uint64_t " << instName << "_eval_cache_out_rst_seen_ver = 0ull;\n";
    os << "  std::uint64_t " << instName << "_eval_cache_out_rst_fp = 0ull;\n";
    os << "  " << cppType(plan.graph.values[fifo.inputIds[4]].type) << " " << instName << "_eval_cache_in_valid{};\n";
    os << "  std::uint64_t " << instName << "_eval_cache_in_valid_ver = 1ull;\n";
    os << "  std::uint64_t " << instName << "_eval_cache_in_valid_seen_ver = 0ull;\n";
    os << "  std::uint64_t " << instName << "_eval_cache_in_valid_fp = 0ull;\n";
    os << "  " << cppType(plan.graph.values[fifo.inputIds[5]].type) << " " << instName << "_eval_cache_in_data{};\n";
    os << "  std::uint64_t " << instName << "_eval_cache_in_data_ver = 1ull;\n";
    os << "  std::uint64_t " << instName << "_eval_cache_in_data_seen_ver = 0ull;\n";
    if (plan.fingerprintInputs.contains(fifo.inputIds[5]))
      os << "  std::uint64_t " << instName << "_eval_cache_in_data_fp = 0ull;\n";
    os << "  " << cppType(plan.graph.values[fifo.inputIds[6]].type) << " " << instName << "_eval_cache_out_ready{};\n";
    os << "  std::uint64_t " << instName << "_eval_cache_out_ready_ver = 1ull;\n";
    os << "  std::uint64_t " << instName << "_eval_cache_out_ready_seen_ver = 0ull;\n";
    os << "  std::uint64_t " << instName << "_eval_cache_out_ready_fp = 0ull;\n";
  }
  for (unsigned id : cdcSyncs) {
    const SimNode &cdc = plan.graph.topNodes[id];
    const SimValue &out = plan.graph.values[cdc.outputIds.front()];
    unsigned w = out.type.width;
    if (w == 0 || w > 64)
      return cdc.op->emitError("C++ emitter only supports cdc_sync widths 1..64 in the prototype");
    os << "  pyc::cpp::pyc_cdc_sync<" << w << ", " << cdc.cdcStages
       << "> " << nt.get(cdc.outputIds.front()) << "_inst;\n";
  }
  os << "\n";

  os << "  struct _pyc_sim_stats_t {\n";
  os << "    std::uint64_t instance_eval_calls = 0;\n";
  os << "    std::uint64_t instance_cache_skips = 0;\n";
  os << "    std::uint64_t primitive_eval_calls = 0;\n";
  os << "    std::uint64_t primitive_cache_skips = 0;\n";
  os << "    std::uint64_t group_eval_calls = 0;\n";
  os << "    std::uint64_t group_cache_skips = 0;\n";
  os << "    std::uint64_t fallback_iterations = 0;\n";
  os << "  };\n";
  os << "  bool _pyc_sim_stats_enable = false;\n";
  os << "  bool _pyc_sim_fast_enable = false;\n";
  os << "  std::string _pyc_sim_stats_path{};\n";
  os << "  _pyc_sim_stats_t _pyc_sim_stats{};\n\n";
  os << "  static bool _pyc_parse_bool_env(const char *name, bool dflt = false) {\n";
  os << "    const char *v = std::getenv(name);\n";
  os << "    if (!v || !*v)\n";
  os << "      return dflt;\n";
  os << "    if (v[0] == '0')\n";
  os << "      return false;\n";
  os << "    if (v[0] == '1')\n";
  os << "      return true;\n";
  os << "    return dflt;\n";
  os << "  }\n\n";
  os << "  void _pyc_init_runtime_controls() {\n";
  os << "    _pyc_sim_stats_enable = _pyc_parse_bool_env(\"PYC_SIM_STATS\", false);\n";
  os << "    _pyc_sim_fast_enable = _pyc_parse_bool_env(\"PYC_SIM_FAST\", false);\n";
  os << "    const char *path = std::getenv(\"PYC_SIM_STATS_PATH\");\n";
  os << "    if (path && *path)\n";
  os << "      _pyc_sim_stats_path = path;\n";
  os << "  }\n\n";
  os << "  void reset_sim_stats() { _pyc_sim_stats = _pyc_sim_stats_t{}; }\n\n";
  os << "  void dump_sim_stats(std::ostream &os) const {\n";
  os << "    os << \"instance_eval_calls=\" << _pyc_sim_stats.instance_eval_calls << \"\\n\";\n";
  os << "    os << \"instance_cache_skips=\" << _pyc_sim_stats.instance_cache_skips << \"\\n\";\n";
  os << "    os << \"primitive_eval_calls=\" << _pyc_sim_stats.primitive_eval_calls << \"\\n\";\n";
  os << "    os << \"primitive_cache_skips=\" << _pyc_sim_stats.primitive_cache_skips << \"\\n\";\n";
  os << "    os << \"group_eval_calls=\" << _pyc_sim_stats.group_eval_calls << \"\\n\";\n";
  os << "    os << \"group_cache_skips=\" << _pyc_sim_stats.group_cache_skips << \"\\n\";\n";
  os << "    os << \"fallback_iterations=\" << _pyc_sim_stats.fallback_iterations << \"\\n\";\n";
  os << "  }\n\n";
  os << "  void dump_sim_stats_to_path(const char *path = nullptr) const {\n";
  os << "    const char *outPath = path;\n";
  os << "    if (!outPath || !*outPath)\n";
  os << "      outPath = _pyc_sim_stats_path.c_str();\n";
  os << "    if (!outPath || !*outPath)\n";
  os << "      return;\n";
  os << "    std::ofstream ofs(outPath, std::ios::out | std::ios::trunc);\n";
  os << "    if (!ofs)\n";
  os << "      return;\n";
  os << "    dump_sim_stats(ofs);\n";
  os << "  }\n\n";

  os << "  void _pyc_validate_primitive_bindings() const {\n";
  for (unsigned id : regs) {
    const SimNode &reg = plan.graph.topNodes[id];
    std::string qName = nt.get(reg.outputIds.front());
    os << "    if (!" << qName << "_inst) { std::cerr << \"pyc null reg binding: " << qName
       << "_inst\" << \"\\n\"; std::abort(); }\n";
  }
  for (unsigned id : syncMems) {
    std::string instName = memoryInstanceName(id);
    os << "    if (!" << instName << ") { std::cerr << \"pyc null sync_mem binding: " << instName
       << "\" << \"\\n\"; std::abort(); }\n";
  }
  for (unsigned id : syncMemDPs) {
    std::string instName = memoryInstanceName(id);
    os << "    if (!" << instName << ") { std::cerr << \"pyc null sync_mem_dp binding: " << instName
       << "\" << \"\\n\"; std::abort(); }\n";
  }
  os << "  }\n\n";

  // Constructor (wire members default-initialize to 0).
  os << "  " << structName << "()";
  bool firstInit = true;
  for (unsigned id : fifos) {
    os << (firstInit ? " :\n" : ",\n");
    firstInit = false;
    os << "      " << fifoInstanceName(id) << "(" << fifoInputName(id, 0)
       << ", " << fifoInputName(id, 1) << ", " << fifoInputName(id, 2)
       << ", " << fifoOutputName(id, 0) << ", " << fifoInputName(id, 3)
       << ", " << fifoOutputName(id, 1) << ", " << fifoInputName(id, 4)
       << ", " << fifoOutputName(id, 2) << ")";
  }
  for (unsigned id : byteMems) {
    const SimNode &mem = plan.graph.topNodes[id];
    os << (firstInit ? " :\n" : ",\n");
    firstInit = false;
    os << "      " << memoryInstanceName(id) << "("
       << graphValueName(mem.inputIds[0]) << ", "
       << graphValueName(mem.inputIds[1]) << ", "
       << graphValueName(mem.inputIds[2]) << ", "
       << graphValueName(mem.outputIds.front());
    for (unsigned i = 3; i < mem.inputIds.size(); ++i)
      os << ", " << graphValueName(mem.inputIds[i]);
    os << ")";
  }
  for (unsigned id : asyncFifos) {
    os << (firstInit ? " :\n" : ",\n");
    firstInit = false;
    os << "      " << fifoInstanceName(id) << "(" << fifoInputName(id, 0)
       << ", " << fifoInputName(id, 1) << ", " << fifoInputName(id, 4)
       << ", " << fifoOutputName(id, 0) << ", " << fifoInputName(id, 5)
       << ", " << fifoInputName(id, 2) << ", " << fifoInputName(id, 3)
       << ", " << fifoOutputName(id, 1) << ", " << fifoInputName(id, 6)
       << ", " << fifoOutputName(id, 2) << ")";
  }
  for (unsigned id : cdcSyncs) {
    const SimNode &cdc = plan.graph.topNodes[id];
    os << (firstInit ? " :\n" : ",\n");
    firstInit = false;
    os << "      " << nt.get(cdc.outputIds.front())
       << "_inst(";
    for (unsigned inputId : cdc.inputIds)
      os << nt.get(inputId) << ", ";
    os << nt.get(cdc.outputIds.front()) << ")";
  }
  os << " {\n";
  for (unsigned id : regs) {
    const SimNode &reg = plan.graph.topNodes[id];
    const SimValue &q = plan.graph.values[reg.outputIds.front()];
    unsigned w = q.type.width;
    if (w == 0)
      return reg.op->emitError("invalid reg width");
    std::string qName = nt.get(reg.outputIds.front());
    if (!q.type.shape.empty())
      os << "    " << qName << "_inst = new pyc::cpp::pyc_vec_reg<"
         << cppType(q.type) << ">(";
    else
      os << "    " << qName << "_inst = new pyc::cpp::pyc_reg<" << w << ">(";
    for (unsigned inputId : reg.inputIds)
      os << nt.get(inputId) << ", ";
    os << qName << ");\n";
  }
  for (unsigned id : syncMems) {
    const SimNode &mem = plan.graph.topNodes[id];
    unsigned addrW = plan.graph.values[mem.inputIds[3]].type.width;
    unsigned dataW = plan.graph.values[mem.outputIds.front()].type.width;
    std::string instName = memoryInstanceName(id);
    os << "    " << instName << " = new pyc::cpp::pyc_sync_mem<" << addrW
       << ", " << dataW << ", " << mem.primitiveDepth << ">(";
    for (unsigned i = 0; i < 4; ++i)
      os << graphValueName(mem.inputIds[i]) << ", ";
    os << graphValueName(mem.outputIds.front());
    for (unsigned i = 4; i < mem.inputIds.size(); ++i)
      os << ", " << graphValueName(mem.inputIds[i]);
    os << ");\n";
  }
  for (unsigned id : syncMemDPs) {
    const SimNode &mem = plan.graph.topNodes[id];
    unsigned addrW = plan.graph.values[mem.inputIds[3]].type.width;
    unsigned dataW = plan.graph.values[mem.outputIds.front()].type.width;
    std::string instName = memoryInstanceName(id);
    os << "    " << instName << " = new pyc::cpp::pyc_sync_mem_dp<"
       << addrW << ", " << dataW << ", " << mem.primitiveDepth << ">(";
    for (unsigned i = 0; i < 4; ++i)
      os << graphValueName(mem.inputIds[i]) << ", ";
    os << graphValueName(mem.outputIds[0]) << ", "
       << graphValueName(mem.inputIds[4]) << ", "
       << graphValueName(mem.inputIds[5]) << ", "
       << graphValueName(mem.outputIds[1]);
    for (unsigned i = 6; i < mem.inputIds.size(); ++i)
      os << ", " << graphValueName(mem.inputIds[i]);
    os << ");\n";
  }
  os << "    _pyc_validate_primitive_bindings();\n";
  os << "    _pyc_init_runtime_controls();\n";
  os << "    #ifdef PYC_ENABLE_CTOR_EVAL\n";
  os << "    eval();\n";
  os << "    #endif\n";
  os << "  }\n\n";

  // Emit fused comb helpers.
  for (auto [i, nodeId] : llvm::enumerate(plan.operationOrder.combs)) {
    unsigned regionId = plan.graph.topNodes[nodeId].combRegionId;
    if (regionId >= plan.graph.combRegions.size())
      return plan.graph.topNodes[nodeId].op->emitError("missing planned simulation comb region");
    auto statements = plan.combRegionStatements.find(regionId);
    if (statements == plan.combRegionStatements.end())
      return plan.graph.topNodes[nodeId].op->emitError("missing planned comb statements");
    if (failed(emitCombMethod(plan.graph, plan.graph.combRegions[regionId],
                              statements->second, plan.combRegionStatements,
                              os, nt,
                              static_cast<unsigned>(i))))
      return failure();
  }
  for (auto [i, nodeIndex] : llvm::enumerate(plan.graph.groupedNodes)) {
    auto activation = plan.groupActivations.find(nodeIndex);
    const GroupActivationPlan *activationPlan =
        activation == plan.groupActivations.end() ? nullptr : &activation->second;
    if (failed(emitSimGroupMethod(plan, nodeIndex, os, nt,
                                  static_cast<unsigned>(i), activationPlan)))
      return failure();
  }

  llvm::DenseMap<unsigned, unsigned> combIndex;
  for (auto [i, nodeId] : llvm::enumerate(plan.operationOrder.combs))
    combIndex.try_emplace(nodeId, static_cast<unsigned>(i));

  auto emitGroupCall = [&](unsigned nodeId, llvm::StringRef indent) {
    unsigned groupIndex = plan.groupByNode.lookup(nodeId);
    if (plan.packedActivationGroups.contains(nodeId)) {
      os << indent << "if (_pyc_group_active_flags[" << groupIndex / 64u
         << "] & (1ull << " << groupIndex % 64u << ")) {\n";
      os << indent << "  _pyc_group_active_flags[" << groupIndex / 64u
         << "] &= ~(1ull << " << groupIndex % 64u << ");\n";
      os << indent << "  eval_sim_group_" << groupIndex << "();\n";
      os << indent << "} else if (_pyc_sim_stats_enable) {\n";
      os << indent << "  _pyc_sim_stats.group_cache_skips++;\n";
      os << indent << "}\n";
    } else {
      os << indent << "eval_sim_group_" << groupIndex << "();\n";
    }
  };

  // eval_comb_pass(): evaluate all combinational ops/assigns.
  //
  // Note: The IR is allowed to have "late" pyc.assign ops (e.g. queue wrappers
  // that defer wiring). To keep C++ simulation correct, eval() runs a small
  // fixed-point iteration that alternates comb evaluation and primitive eval.
  os << "  inline void eval_comb_pass() {\n";
  const auto &ordered = plan.combNodeOrder;

  for (unsigned nodeId : ordered) {
    const SimNode &node = plan.graph.topNodes[nodeId];
    const SimNodeAction &action = plan.nodeActions[nodeId];
    Operation *op = node.op;
    if (action.kind == SimNodeActionKind::Group) {
      emitGroupCall(nodeId, "    ");
      continue;
    }
    if (action.kind == SimNodeActionKind::Assign) {
      os << "    " << nt.get(action.targetId)
         << " = " << nt.get(action.sourceId)
         << ";\n";
      continue;
    }
    if (action.kind == SimNodeActionKind::CombRegion) {
      os << "    eval_comb_" << combIndex.lookup(nodeId) << "();\n";
      continue;
    }
    if (action.kind == SimNodeActionKind::Assert) {
      os << "    if (!"
         << nt.get(action.conditionId)
         << ".toBool()) { std::cerr << "
         << cppStringLiteral(action.message)
         << " << \"\\n\"; std::abort(); }\n";
      continue;
    }
    if (action.kind == SimNodeActionKind::Expression) {
      if (failed(emitGraphExpr(plan.graph,
                               plan.graph.expressions[action.expressionId],
                               os, nt)))
        return failure();
      continue;
    }
    if (action.kind == SimNodeActionKind::Fifo ||
        action.kind == SimNodeActionKind::AsyncFifo ||
        action.kind == SimNodeActionKind::ByteMem ||
        action.kind == SimNodeActionKind::Instance ||
        action.kind == SimNodeActionKind::Skip) {
      // Primitives are evaluated in eval(), and regs only tick.
      continue;
    }
    return op->emitError("unsupported op for C++ emission: ") << op->getName();
  }
  os << "  }\n\n";

  const auto &fullOrdered = plan.evalNodeOrder;
  bool hasFullTopo = plan.evalTopological;

  llvm::SmallVector<std::string> instanceEvalHelperNames;
  instanceEvalHelperNames.reserve(instInfos.size());
  for (unsigned idx = 0; idx < instInfos.size(); ++idx) {
    std::string helperName = "eval_instance_cached_" + std::to_string(idx);
    instanceEvalHelperNames.push_back(helperName);
  }

  auto emitInstanceEvalHelperDefinition = [&](const InstInfo &ii, llvm::StringRef helperName) {
    const SimNode &inst = plan.graph.topNodes[ii.nodeId];
    bool usePackedCache = usePackedInstanceEvalCache(ii);
    os << "  inline bool " << helperName << "() {\n";
    os << "    bool _pyc_inst_changed = false;\n";
    os << "    #ifdef PYC_DISABLE_INSTANCE_EVAL_CACHE\n";
    for (unsigned i = 0; i < inst.inputIds.size(); ++i) {
      std::string inValue = graphValueName(inst.inputIds[i]);
      os << "    " << ii.member << "->" << ii.inPorts[i] << " = " << inValue << ";\n";
    }
    os << "    " << ii.member << "->eval();\n";
    os << "    if (_pyc_sim_stats_enable) _pyc_sim_stats.instance_eval_calls++;\n";
    os << "    _pyc_inst_changed = true;\n";
    os << "    #else\n";
    std::string changedFlag = ii.member + "_eval_cache_changed";
    os << "    bool " << changedFlag << " = !" << ii.member << "_eval_cache_valid;\n";
    if (usePackedCache) {
      os << "    std::array<std::uint64_t, " << instancePackedCacheWordCount(ii) << "> _pyc_inputs{};\n";
      os << "    std::size_t _pyc_inputs_off = 0;\n";
      for (unsigned i = 0; i < inst.inputIds.size(); ++i) {
        std::string inValue = graphValueName(inst.inputIds[i]);
        os << "    pyc::cpp::appendPackedWireWords(_pyc_inputs, _pyc_inputs_off, " << inValue << ");\n";
      }
      os << "    if (!" << changedFlag << " && (" << ii.member << "_eval_cache_words != _pyc_inputs)) " << changedFlag
         << " = true;\n";
      os << "    if (" << changedFlag << ") {\n";
      for (unsigned i = 0; i < inst.inputIds.size(); ++i) {
        std::string inValue = graphValueName(inst.inputIds[i]);
        os << "      " << ii.member << "->" << ii.inPorts[i] << " = " << inValue << ";\n";
      }
      os << "      " << ii.member << "->eval();\n";
      os << "      if (_pyc_sim_stats_enable) _pyc_sim_stats.instance_eval_calls++;\n";
      os << "      " << ii.member << "_eval_cache_words = _pyc_inputs;\n";
      os << "    } else {\n";
      os << "      if (_pyc_sim_stats_enable) _pyc_sim_stats.instance_cache_skips++;\n";
      os << "    }\n";
      os << "    _pyc_inst_changed = " << changedFlag << ";\n";
      os << "    " << ii.member << "_eval_cache_valid = true;\n";
      os << "    #endif\n";
    } else {
      os << "    #ifndef PYC_DISABLE_VERSIONED_INPUT_CACHE\n";
      for (unsigned i = 0; i < inst.inputIds.size(); ++i) {
        std::string cacheName = ii.member + "_eval_cache_in_" + std::to_string(i);
        std::string inValue = graphValueName(inst.inputIds[i]);
        std::string verName = ii.member + "_eval_cache_in_ver_" + std::to_string(i);
        std::string seenName = ii.member + "_eval_cache_in_seen_ver_" + std::to_string(i);
        os << "    if (" << ii.member << "_eval_cache_valid) {\n";
        if (plan.fingerprintInputs.contains(inst.inputIds[i])) {
          std::string fpName = ii.member + "_eval_cache_in_fp_" + std::to_string(i);
          os << "      std::uint64_t _pyc_fp_" << i << " = static_cast<std::uint64_t>(" << inValue << ".value());\n";
          os << "      if (" << fpName << " != _pyc_fp_" << i << ") {\n";
          os << "        " << fpName << " = _pyc_fp_" << i << ";\n";
          os << "        " << cacheName << " = " << inValue << ";\n";
          os << "        ++" << verName << ";\n";
          os << "      }\n";
        } else {
          os << "      if (" << cacheName << " != " << inValue << ") {\n";
          os << "        " << cacheName << " = " << inValue << ";\n";
          os << "        ++" << verName << ";\n";
          os << "      }\n";
        }
        os << "    } else {\n";
        os << "      " << cacheName << " = " << inValue << ";\n";
        if (plan.fingerprintInputs.contains(inst.inputIds[i])) {
          std::string fpName = ii.member + "_eval_cache_in_fp_" + std::to_string(i);
          os << "      " << fpName << " = static_cast<std::uint64_t>(" << inValue << ".value());\n";
        }
        os << "      ++" << verName << ";\n";
        os << "    }\n";
        os << "    if (!" << changedFlag << " && (" << seenName << " != " << verName << ")) " << changedFlag
           << " = true;\n";
        os << "    " << seenName << " = " << verName << ";\n";
      }
      os << "    if (" << changedFlag << ") {\n";
      for (unsigned i = 0; i < inst.inputIds.size(); ++i) {
        std::string cacheName = ii.member + "_eval_cache_in_" + std::to_string(i);
        os << "      " << ii.member << "->" << ii.inPorts[i] << " = " << cacheName << ";\n";
      }
      os << "      " << ii.member << "->eval();\n";
      os << "      if (_pyc_sim_stats_enable) _pyc_sim_stats.instance_eval_calls++;\n";
      os << "    } else {\n";
      os << "      if (_pyc_sim_stats_enable) _pyc_sim_stats.instance_cache_skips++;\n";
      os << "    }\n";
      os << "    #else\n";
      for (unsigned i = 0; i < inst.inputIds.size(); ++i) {
        std::string cacheName = ii.member + "_eval_cache_in_" + std::to_string(i);
        std::string inValue = graphValueName(inst.inputIds[i]);
        os << "    if (!" << changedFlag << " && (" << cacheName << " != " << inValue << ")) " << changedFlag
           << " = true;\n";
      }
      os << "    if (" << changedFlag << ") {\n";
      for (unsigned i = 0; i < inst.inputIds.size(); ++i) {
        std::string cacheName = ii.member + "_eval_cache_in_" + std::to_string(i);
        std::string inValue = graphValueName(inst.inputIds[i]);
        os << "      " << ii.member << "->" << ii.inPorts[i] << " = " << inValue << ";\n";
        os << "      " << cacheName << " = " << inValue << ";\n";
      }
      os << "      " << ii.member << "->eval();\n";
      os << "      if (_pyc_sim_stats_enable) _pyc_sim_stats.instance_eval_calls++;\n";
      os << "    } else {\n";
      os << "      if (_pyc_sim_stats_enable) _pyc_sim_stats.instance_cache_skips++;\n";
      os << "    }\n";
      os << "    #endif\n";
      os << "    _pyc_inst_changed = " << changedFlag << ";\n";
      os << "    " << ii.member << "_eval_cache_valid = true;\n";
      os << "    #endif\n";
    }
    for (unsigned i = 0; i < inst.outputIds.size(); ++i)
      os << "    " << graphValueName(inst.outputIds[i]) << " = " << ii.member << "->" << ii.outPorts[i] << ";\n";
    os << "    return _pyc_inst_changed;\n";
    os << "  }\n\n";
  };

  for (unsigned idx = 0; idx < instInfos.size(); ++idx)
    emitInstanceEvalHelperDefinition(instInfos[idx], instanceEvalHelperNames[idx]);

  auto emitInstanceEvalWithCache =
      [&](const InstInfo &ii, llvm::StringRef indent, llvm::StringRef changedAnyVar = llvm::StringRef()) {
    auto it = instIndex.find(ii.nodeId);
    if (it == instIndex.end())
      return;
    llvm::StringRef helperName = instanceEvalHelperNames[it->second];
    if (!changedAnyVar.empty()) {
      os << indent << "if (" << helperName << "()) " << changedAnyVar << " = true;\n";
    } else {
      os << indent << helperName << "();\n";
    }
  };

  auto emitFifoEvalWithCache =
      [&](unsigned nodeId, llvm::StringRef indent, llvm::StringRef changedAnyVar = llvm::StringRef()) {
    const SimNode &fifo = plan.graph.topNodes[nodeId];
    std::string instName = fifoInstanceName(nodeId);
    std::string changedFlag = instName + "_eval_cache_changed";
    std::string inValid = fifoInputName(nodeId, 2);
    std::string inData = fifoInputName(nodeId, 3);
    std::string outReady = fifoInputName(nodeId, 4);
    os << indent << "#ifdef PYC_DISABLE_PRIMITIVE_EVAL_CACHE\n";
    os << indent << instName << ".eval();\n";
    os << indent << "if (_pyc_sim_stats_enable) _pyc_sim_stats.primitive_eval_calls++;\n";
    if (!changedAnyVar.empty())
      os << indent << changedAnyVar << " = true;\n";
    os << indent << "#else\n";
    os << indent << "#ifndef PYC_DISABLE_VERSIONED_INPUT_CACHE\n";
    os << indent << "bool " << changedFlag << " = !" << instName << "_eval_cache_valid;\n";
    os << indent << "if (" << instName << "_eval_cache_valid) {\n";
    os << indent << "  std::uint64_t _pyc_fp_v = static_cast<std::uint64_t>(" << inValid << ".value());\n";
    os << indent << "  if (" << instName << "_eval_cache_in_valid_fp != _pyc_fp_v) {\n";
    os << indent << "    " << instName << "_eval_cache_in_valid_fp = _pyc_fp_v;\n";
    os << indent << "    " << instName << "_eval_cache_in_valid = " << inValid << ";\n";
    os << indent << "    ++" << instName << "_eval_cache_in_valid_ver;\n";
    os << indent << "  }\n";
    if (plan.fingerprintInputs.contains(fifo.inputIds[3])) {
      os << indent << "  std::uint64_t _pyc_fp_d = static_cast<std::uint64_t>(" << inData << ".value());\n";
      os << indent << "  if (" << instName << "_eval_cache_in_data_fp != _pyc_fp_d) {\n";
      os << indent << "    " << instName << "_eval_cache_in_data_fp = _pyc_fp_d;\n";
      os << indent << "    " << instName << "_eval_cache_in_data = " << inData << ";\n";
      os << indent << "    ++" << instName << "_eval_cache_in_data_ver;\n";
      os << indent << "  }\n";
    } else {
      os << indent << "  if (" << instName << "_eval_cache_in_data != " << inData << ") {\n";
      os << indent << "    " << instName << "_eval_cache_in_data = " << inData << ";\n";
      os << indent << "    ++" << instName << "_eval_cache_in_data_ver;\n";
      os << indent << "  }\n";
    }
    os << indent << "  std::uint64_t _pyc_fp_r = static_cast<std::uint64_t>(" << outReady << ".value());\n";
    os << indent << "  if (" << instName << "_eval_cache_out_ready_fp != _pyc_fp_r) {\n";
    os << indent << "    " << instName << "_eval_cache_out_ready_fp = _pyc_fp_r;\n";
    os << indent << "    " << instName << "_eval_cache_out_ready = " << outReady << ";\n";
    os << indent << "    ++" << instName << "_eval_cache_out_ready_ver;\n";
    os << indent << "  }\n";
    os << indent << "} else {\n";
    os << indent << "  " << instName << "_eval_cache_in_valid = " << inValid << ";\n";
    os << indent << "  " << instName << "_eval_cache_in_valid_fp = static_cast<std::uint64_t>(" << inValid << ".value());\n";
    os << indent << "  ++" << instName << "_eval_cache_in_valid_ver;\n";
    os << indent << "  " << instName << "_eval_cache_in_data = " << inData << ";\n";
    if (plan.fingerprintInputs.contains(fifo.inputIds[3]))
      os << indent << "  " << instName << "_eval_cache_in_data_fp = static_cast<std::uint64_t>(" << inData << ".value());\n";
    os << indent << "  ++" << instName << "_eval_cache_in_data_ver;\n";
    os << indent << "  " << instName << "_eval_cache_out_ready = " << outReady << ";\n";
    os << indent << "  " << instName << "_eval_cache_out_ready_fp = static_cast<std::uint64_t>(" << outReady
       << ".value());\n";
    os << indent << "  ++" << instName << "_eval_cache_out_ready_ver;\n";
    os << indent << "}\n";
    os << indent << "if (!" << changedFlag << " && (" << instName << "_eval_cache_in_valid_seen_ver != " << instName
       << "_eval_cache_in_valid_ver)) " << changedFlag << " = true;\n";
    os << indent << "if (!" << changedFlag << " && (" << instName << "_eval_cache_in_data_seen_ver != " << instName
       << "_eval_cache_in_data_ver)) " << changedFlag << " = true;\n";
    os << indent << "if (!" << changedFlag << " && (" << instName << "_eval_cache_out_ready_seen_ver != " << instName
       << "_eval_cache_out_ready_ver)) " << changedFlag << " = true;\n";
    os << indent << instName << "_eval_cache_in_valid_seen_ver = " << instName << "_eval_cache_in_valid_ver;\n";
    os << indent << instName << "_eval_cache_in_data_seen_ver = " << instName << "_eval_cache_in_data_ver;\n";
    os << indent << instName << "_eval_cache_out_ready_seen_ver = " << instName << "_eval_cache_out_ready_ver;\n";
    os << indent << "#else\n";
    os << indent << "bool " << changedFlag << " = !" << instName << "_eval_cache_valid;\n";
    os << indent << "if (!" << changedFlag << " && (" << instName << "_eval_cache_in_valid != " << inValid << ")) "
       << changedFlag << " = true;\n";
    os << indent << "if (!" << changedFlag << " && (" << instName << "_eval_cache_in_data != " << inData << ")) "
       << changedFlag << " = true;\n";
    os << indent << "if (!" << changedFlag << " && (" << instName << "_eval_cache_out_ready != " << outReady
       << ")) " << changedFlag << " = true;\n";
    os << indent << "#endif\n";
    os << indent << "if (" << changedFlag << ") {\n";
    os << indent << "  " << instName << ".eval();\n";
    os << indent << "  if (_pyc_sim_stats_enable) _pyc_sim_stats.primitive_eval_calls++;\n";
    os << indent << "} else {\n";
    os << indent << "  if (_pyc_sim_stats_enable) _pyc_sim_stats.primitive_cache_skips++;\n";
    os << indent << "}\n";
    if (!changedAnyVar.empty())
      os << indent << "if (" << changedFlag << ") " << changedAnyVar << " = true;\n";
    os << indent << "#ifdef PYC_DISABLE_VERSIONED_INPUT_CACHE\n";
    os << indent << "if (" << changedFlag << ") {\n";
    os << indent << "  " << instName << "_eval_cache_in_valid = " << inValid << ";\n";
    os << indent << "  " << instName << "_eval_cache_in_data = " << inData << ";\n";
    os << indent << "  " << instName << "_eval_cache_out_ready = " << outReady << ";\n";
    os << indent << "}\n";
    os << indent << "#endif\n";
    os << indent << instName << "_eval_cache_valid = true;\n";
    os << indent << "#endif\n";
  };

  auto emitAsyncFifoEvalWithCache =
      [&](unsigned nodeId, llvm::StringRef indent, llvm::StringRef changedAnyVar = llvm::StringRef()) {
    std::string instName = fifoInstanceName(nodeId);
    std::string changedFlag = instName + "_eval_cache_changed";
    std::string inRst = fifoInputName(nodeId, 1);
    std::string outRst = fifoInputName(nodeId, 3);
    std::string inValid = fifoInputName(nodeId, 4);
    std::string inData = fifoInputName(nodeId, 5);
    std::string outReady = fifoInputName(nodeId, 6);
    os << indent << "#ifdef PYC_DISABLE_PRIMITIVE_EVAL_CACHE\n";
    os << indent << instName << ".eval();\n";
    os << indent << "if (_pyc_sim_stats_enable) _pyc_sim_stats.primitive_eval_calls++;\n";
    if (!changedAnyVar.empty())
      os << indent << changedAnyVar << " = true;\n";
    os << indent << "#else\n";
    os << indent << "bool " << changedFlag << " = !" << instName << "_eval_cache_valid;\n";
    os << indent << "if (!" << changedFlag << " && (" << instName << "_eval_cache_in_rst != " << inRst << ")) "
       << changedFlag << " = true;\n";
    os << indent << "if (!" << changedFlag << " && (" << instName << "_eval_cache_out_rst != " << outRst << ")) "
       << changedFlag << " = true;\n";
    os << indent << "if (!" << changedFlag << " && (" << instName << "_eval_cache_in_valid != " << inValid
       << ")) " << changedFlag << " = true;\n";
    os << indent << "if (!" << changedFlag << " && (" << instName << "_eval_cache_in_data != " << inData
       << ")) " << changedFlag << " = true;\n";
    os << indent << "if (!" << changedFlag << " && (" << instName << "_eval_cache_out_ready != " << outReady
       << ")) " << changedFlag << " = true;\n";
    os << indent << "if (" << changedFlag << ") {\n";
    os << indent << "  " << instName << ".eval();\n";
    os << indent << "  if (_pyc_sim_stats_enable) _pyc_sim_stats.primitive_eval_calls++;\n";
    os << indent << "} else {\n";
    os << indent << "  if (_pyc_sim_stats_enable) _pyc_sim_stats.primitive_cache_skips++;\n";
    os << indent << "}\n";
    if (!changedAnyVar.empty())
      os << indent << "if (" << changedFlag << ") " << changedAnyVar << " = true;\n";
    os << indent << "if (" << changedFlag << ") {\n";
    os << indent << "  " << instName << "_eval_cache_in_rst = " << inRst << ";\n";
    os << indent << "  " << instName << "_eval_cache_out_rst = " << outRst << ";\n";
    os << indent << "  " << instName << "_eval_cache_in_valid = " << inValid << ";\n";
    os << indent << "  " << instName << "_eval_cache_in_data = " << inData << ";\n";
    os << indent << "  " << instName << "_eval_cache_out_ready = " << outReady << ";\n";
    os << indent << "}\n";
    os << indent << instName << "_eval_cache_valid = true;\n";
    os << indent << "#endif\n";
  };

  auto emitByteMemEvalWithCache =
      [&](unsigned nodeId, llvm::StringRef indent, llvm::StringRef changedAnyVar = llvm::StringRef()) {
    const SimNode &mem = plan.graph.topNodes[nodeId];
    std::string instName = memoryInstanceName(nodeId);
    std::string changedFlag = instName + "_eval_cache_changed";
    std::string rst = graphValueName(mem.inputIds[1]);
    std::string raddr = graphValueName(mem.inputIds[2]);
    std::string wvalid = graphValueName(mem.inputIds[3]);
    std::string waddr = graphValueName(mem.inputIds[4]);
    std::string wdata = graphValueName(mem.inputIds[5]);
    std::string wstrb = graphValueName(mem.inputIds[6]);
    os << indent << "#ifdef PYC_DISABLE_PRIMITIVE_EVAL_CACHE\n";
    os << indent << instName << ".eval();\n";
    os << indent << "if (_pyc_sim_stats_enable) _pyc_sim_stats.primitive_eval_calls++;\n";
    if (!changedAnyVar.empty())
      os << indent << changedAnyVar << " = true;\n";
    os << indent << "#else\n";
    os << indent << "bool " << changedFlag << " = !" << instName << "_eval_cache_valid;\n";
    os << indent << "if (!" << changedFlag << " && (" << instName << "_eval_cache_rst != " << rst << ")) "
       << changedFlag << " = true;\n";
    os << indent << "if (!" << changedFlag << " && (" << instName << "_eval_cache_raddr != " << raddr << ")) "
       << changedFlag << " = true;\n";
    os << indent << "if (!" << changedFlag << " && (" << instName << "_eval_cache_wvalid != " << wvalid << ")) "
       << changedFlag << " = true;\n";
    os << indent << "if (!" << changedFlag << " && (" << instName << "_eval_cache_waddr != " << waddr << ")) "
       << changedFlag << " = true;\n";
    os << indent << "if (!" << changedFlag << " && (" << instName << "_eval_cache_wdata != " << wdata << ")) "
       << changedFlag << " = true;\n";
    os << indent << "if (!" << changedFlag << " && (" << instName << "_eval_cache_wstrb != " << wstrb << ")) "
       << changedFlag << " = true;\n";
    os << indent << "if (" << changedFlag << ") {\n";
    os << indent << "  " << instName << ".eval();\n";
    os << indent << "  if (_pyc_sim_stats_enable) _pyc_sim_stats.primitive_eval_calls++;\n";
    os << indent << "} else {\n";
    os << indent << "  if (_pyc_sim_stats_enable) _pyc_sim_stats.primitive_cache_skips++;\n";
    os << indent << "}\n";
    if (!changedAnyVar.empty())
      os << indent << "if (" << changedFlag << ") " << changedAnyVar << " = true;\n";
    os << indent << "if (" << changedFlag << ") {\n";
    os << indent << "  " << instName << "_eval_cache_rst = " << rst << ";\n";
    os << indent << "  " << instName << "_eval_cache_raddr = " << raddr << ";\n";
    os << indent << "  " << instName << "_eval_cache_wvalid = " << wvalid << ";\n";
    os << indent << "  " << instName << "_eval_cache_waddr = " << waddr << ";\n";
    os << indent << "  " << instName << "_eval_cache_wdata = " << wdata << ";\n";
    os << indent << "  " << instName << "_eval_cache_wstrb = " << wstrb << ";\n";
    os << indent << "}\n";
    os << indent << instName << "_eval_cache_valid = true;\n";
    os << indent << "#endif\n";
  };

  auto emitEvalNode =
      [&](unsigned nodeId, llvm::StringRef indent, llvm::StringRef changedAnyVar = llvm::StringRef()) -> LogicalResult {
    const SimNode &node = plan.graph.topNodes[nodeId];
    const SimNodeAction &action = plan.nodeActions[nodeId];
    Operation *op = node.op;
    if (action.kind == SimNodeActionKind::Group) {
      emitGroupCall(nodeId, indent);
      return success();
    }
    if (action.kind == SimNodeActionKind::Fifo) {
      emitFifoEvalWithCache(nodeId, indent, changedAnyVar);
      return success();
    }
    if (action.kind == SimNodeActionKind::AsyncFifo) {
      emitAsyncFifoEvalWithCache(nodeId, indent, changedAnyVar);
      return success();
    }
    if (action.kind == SimNodeActionKind::ByteMem) {
      emitByteMemEvalWithCache(nodeId, indent, changedAnyVar);
      return success();
    }
    if (action.kind == SimNodeActionKind::Instance) {
      auto it = instIndex.find(nodeId);
      if (it == instIndex.end())
        return op->emitError("internal error: missing instance metadata");
      auto &ii = instInfos[it->second];
      emitInstanceEvalWithCache(ii, indent, changedAnyVar);
      return success();
    }
    if (action.kind == SimNodeActionKind::Assert) {
      os << indent << "if (!"
         << nt.get(action.conditionId)
         << ".toBool()) { std::cerr << "
         << cppStringLiteral(action.message)
         << " << \"\\n\"; std::abort(); }\n";
      return success();
    }
    if (action.kind == SimNodeActionKind::Assign) {
      os << indent << nt.get(action.targetId)
         << " = " << nt.get(action.sourceId)
         << ";\n";
      return success();
    }
    if (action.kind == SimNodeActionKind::CombRegion) {
      os << indent << "eval_comb_" << combIndex.lookup(nodeId) << "();\n";
      return success();
    }
    if (action.kind == SimNodeActionKind::Expression) {
      if (failed(emitGraphExpr(plan.graph,
                               plan.graph.expressions[action.expressionId],
                               os, nt)))
        return failure();
      return success();
    }
    return op->emitError("unsupported op for C++ emission: ") << op->getName();
  };

  const auto &sccPlans = plan.sccOrder;
  bool hasSccWorklistPlan = plan.useSccWorklist;

  // eval(): attempt a single-pass topological netlist schedule; if the graph has
  // cycles, fall back to a bounded fixed-point iteration.
  if (!hasFullTopo) {
    unsigned numPrims = plan.primitiveCount;

    std::vector<std::string> primGroupMethods;
    if (numPrims > 0) {
      for (auto [groupIdx, chunk] : llvm::enumerate(plan.primitiveEvalChunks)) {
        std::string methodName = "eval_prim_group_" + std::to_string(groupIdx);
        primGroupMethods.push_back(methodName);
        os << "  inline void " << methodName << "(bool &_pyc_prim_changed) {\n";
        for (unsigned id : chunk)
          if (failed(emitEvalNode(id, "    ", "_pyc_prim_changed")))
            return failure();
        os << "  }\n\n";
      }
    }

    os << "  inline void eval_fixpoint_fallback_path() {\n";
    if (numPrims > 0) {
      os << "    for (unsigned _i = 0; _i < " << plan.fallbackIterationLimit << "u; ++_i) {\n";
      os << "      if (_pyc_sim_stats_enable) _pyc_sim_stats.fallback_iterations++;\n";
      os << "      bool _pyc_prim_changed = false;\n";
      for (const std::string &methodName : primGroupMethods)
        os << "      " << methodName << "(_pyc_prim_changed);\n";
      os << "      eval_comb_pass();\n";
      os << "      if (!_pyc_prim_changed) break;\n";
      os << "    }\n";
    }
    os << "  }\n\n";

    if (hasSccWorklistPlan && numPrims > 0) {
      std::vector<std::string> sccCompMethods;
      sccCompMethods.reserve(sccPlans.size());
      const unsigned kSccNodeChunk = plan.sccChunkNodes;
      unsigned compOrdinal = 0;
      for (const auto &comp : sccPlans) {
        std::string methodName = "eval_scc_comp_" + std::to_string(compOrdinal++);
        std::vector<std::string> partMethods;
        partMethods.reserve((comp.nodeIds.size() + kSccNodeChunk - 1) / kSccNodeChunk);

        for (size_t begin = 0, partIdx = 0; begin < comp.nodeIds.size(); begin += kSccNodeChunk, ++partIdx) {
          size_t end = std::min(comp.nodeIds.size(), begin + static_cast<size_t>(kSccNodeChunk));
          std::string partName = methodName + "_part_" + std::to_string(partIdx);
          partMethods.push_back(partName);
          if (comp.cyclic) {
            os << "  inline void " << partName << "(bool &_pyc_prim_changed) {\n";
            for (size_t i = begin; i < end; ++i)
              if (failed(emitEvalNode(comp.nodeIds[i],
                                      "    ", "_pyc_prim_changed")))
                return failure();
          } else {
            os << "  inline void " << partName << "() {\n";
            for (size_t i = begin; i < end; ++i)
              if (failed(emitEvalNode(comp.nodeIds[i],
                                      "    ")))
                return failure();
          }
          os << "  }\n\n";
        }

        sccCompMethods.push_back(methodName);
        os << "  inline void " << methodName << "() {\n";
        if (comp.cyclic) {
          unsigned iterCap = comp.iterationLimit;
          os << "    bool _pyc_converged = false;\n";
          os << "    for (unsigned _pyc_iter = 0; _pyc_iter < " << iterCap << "u; ++_pyc_iter) {\n";
          os << "      if (_pyc_sim_stats_enable) _pyc_sim_stats.fallback_iterations++;\n";
          os << "      bool _pyc_prim_changed = false;\n";
          for (const std::string &partName : partMethods)
            os << "      " << partName << "(_pyc_prim_changed);\n";
          os << "      if (!_pyc_prim_changed) { _pyc_converged = true; break; }\n";
          os << "    }\n";
          os << "    if (!_pyc_converged) {\n";
          os << "      std::cerr << \"pyc SCC fallback failed to converge\" << \"\\n\";\n";
          os << "      std::abort();\n";
          os << "    }\n";
        } else {
          for (const std::string &partName : partMethods)
            os << "    " << partName << "();\n";
        }
        os << "  }\n\n";
      }

      os << "  inline void eval_fast_scc_path() {\n";
      for (const std::string &methodName : sccCompMethods)
        os << "    " << methodName << "();\n";
      os << "  }\n\n";
    }
  }

  std::vector<std::string> topoEvalMethods;
  if (hasFullTopo && !fullOrdered.empty()) {
    unsigned evalTopoChunkNodes = plan.evalChunkNodes;
    if (fullOrdered.size() > evalTopoChunkNodes) {
      topoEvalMethods.reserve((fullOrdered.size() + evalTopoChunkNodes - 1) / evalTopoChunkNodes);
      for (unsigned begin = 0, chunkIdx = 0; begin < fullOrdered.size(); begin += evalTopoChunkNodes, ++chunkIdx) {
        unsigned end = std::min<unsigned>(static_cast<unsigned>(fullOrdered.size()), begin + evalTopoChunkNodes);
        std::string methodName = "eval_topo_part_" + std::to_string(chunkIdx);
        topoEvalMethods.push_back(methodName);
        os << "  inline void " << methodName << "() {\n";
        for (unsigned i = begin; i < end; ++i)
          if (failed(emitEvalNode(fullOrdered[i], "    ")))
            return failure();
        os << "  }\n\n";
      }
    }
  }

  os << "  void eval() {\n";
  if (hasFullTopo) {
    if (!topoEvalMethods.empty()) {
      for (const std::string &methodName : topoEvalMethods)
        os << "    " << methodName << "();\n";
    } else {
      for (unsigned nodeId : fullOrdered)
        if (failed(emitEvalNode(nodeId, "    ")))
          return failure();
    }
  } else {
    os << "    eval_comb_pass();\n";
    unsigned numPrims = plan.primitiveCount;
    if (hasSccWorklistPlan && numPrims > 0) {
      os << "    if (_pyc_sim_fast_enable) {\n";
      os << "      #ifndef PYC_DISABLE_SCC_WORKLIST_EVAL\n";
      os << "      eval_fast_scc_path();\n";
      os << "      #else\n";
      os << "      eval_fixpoint_fallback_path();\n";
      os << "      #endif\n";
      os << "    } else {\n";
      os << "      eval_fixpoint_fallback_path();\n";
      os << "    }\n";
    } else {
      os << "    eval_fixpoint_fallback_path();\n";
    }
  }

  // Connect return values to output ports.
  for (auto [i, id] : llvm::enumerate(plan.graph.outputValueIds))
    os << "    " << outNames[i] << " = "
       << nt.get(id) << ";\n";

  os << "  }\n\n";

  // tick_compute/tick_commit: two-phase sequential update (hierarchy-aware).
  //
  // Large designs can produce enormous tick bodies (notably JanusBccBackendCompat), which
  // makes a single translation unit fragile and slow to compile. Split tick into helper
  // parts so --cpp-split=module can shard tick across multiple .cpp files.
  auto emitTickComputePart = [&](const llvm::SmallVector<unsigned> &chunk,
                                 unsigned partIdx) {
    os << "  inline void tick_compute_part_" << partIdx << "() {\n";
    // Sub-modules (inputs + tick_compute).
    for (unsigned nodeId : chunk) {
      const auto &ii = instInfos[instIndex.lookup(nodeId)];
      const SimNode &inst = plan.graph.topNodes[ii.nodeId];
      for (unsigned j = 0; j < inst.inputIds.size(); ++j)
        os << "    " << ii.member << "->" << ii.inPorts[j] << " = " << graphValueName(inst.inputIds[j]) << ";\n";
      os << "    " << ii.member << "->tick_compute();\n";
    }
    os << "  }\n\n";
  };

  auto emitTickCommitPart = [&](const llvm::SmallVector<unsigned> &chunk,
                                unsigned partIdx) {
    os << "  inline void tick_commit_part_" << partIdx << "() {\n";
    // Sub-modules.
    for (unsigned nodeId : chunk)
      os << "    " << instInfos[instIndex.lookup(nodeId)].member << "->tick_commit();\n";
    os << "  }\n\n";
  };

  // Emit chunked submodule tick helpers.
  unsigned subParts = plan.instanceTickChunks.size();
  for (auto [part, chunk] : llvm::enumerate(plan.instanceTickChunks))
    emitTickComputePart(chunk, static_cast<unsigned>(part));
  for (auto [part, chunk] : llvm::enumerate(plan.instanceTickChunks))
    emitTickCommitPart(chunk, static_cast<unsigned>(part));

  os << "  void tick_compute() {\n";
  if (!instInfos.empty()) {
    os << "    // Sub-modules.\n";
    for (unsigned i = 0; i < subParts; ++i)
      os << "    tick_compute_part_" << i << "();\n";
  }
  os << "    // Local sequential primitives.\n";
  auto regInstanceName = [&](unsigned id) {
    const SimNode &reg = plan.graph.topNodes[id];
    return nt.get(reg.outputIds.front()) + "_inst";
  };
  for (const SimTickAction &action : plan.localTickComputeActions) {
    unsigned id = action.id;
    switch (action.kind) {
    case SimTickActionKind::ResetGroup: {
      const ResetGroupPlan &group = plan.resetGroups[id];
      std::string suffix = std::to_string(id);
      std::string now = "_pyc_reset_group_clk_now_" + suffix;
      std::string edge = "_pyc_reset_group_edge_" + suffix;
      std::string previous = "_pyc_reset_group_clk_prev_" + suffix;
      os << "    bool " << now << " = " << nt.get(group.clockValueId) << ".toBool();\n";
      os << "    bool " << edge << " = !" << previous << " && " << now << ";\n";
      os << "    " << previous << " = " << now << ";\n";
      os << "    if (" << edge << ") {\n";
      os << "      if (" << nt.get(group.resetValueId) << ".toBool()) {\n";
      for (unsigned regId : group.regNodeIds)
        os << "        " << regInstanceName(regId) << "->posedge_reset_compute();\n";
      os << "      } else {\n";
      for (unsigned regId : group.regNodeIds)
        os << "        " << regInstanceName(regId) << "->posedge_data_compute();\n";
      os << "      }\n";
      os << "    } else if (" << now << ") {\n";
      for (unsigned regId : group.regNodeIds)
        os << "      " << regInstanceName(regId) << "->noedge_update();\n";
      os << "    } else {\n";
      for (unsigned regId : group.regNodeIds)
        os << "      " << regInstanceName(regId) << "->negedge_update();\n";
      os << "    }\n";
      break;
    }
    case SimTickActionKind::Reg:
      os << "    " << regInstanceName(id) << "->tick_compute();\n";
      break;
    case SimTickActionKind::Fifo:
    case SimTickActionKind::AsyncFifo:
      os << "    " << fifoInstanceName(id) << ".tick_compute();\n";
      break;
    case SimTickActionKind::ByteMem:
      os << "    " << memoryInstanceName(id) << ".tick_compute();\n";
      break;
    case SimTickActionKind::SyncMem:
    case SimTickActionKind::SyncMemDP:
      os << "    " << memoryInstanceName(id) << "->tick_compute();\n";
      break;
    case SimTickActionKind::CdcSync:
      os << "    " << nt.get(plan.graph.topNodes[id].outputIds.front())
         << "_inst.tick_compute();\n";
      break;
    }
  }
  os << "  }\n\n";

  os << "  void tick_commit() {\n";
  if (!instInfos.empty()) {
    os << "    // Sub-modules.\n";
    for (unsigned i = 0; i < subParts; ++i)
      os << "    tick_commit_part_" << i << "();\n";
  }
  os << "    // Local sequential primitives.\n";
  for (const SimTickAction &action : plan.localTickCommitActions) {
    unsigned id = action.id;
    switch (action.kind) {
    case SimTickActionKind::ResetGroup:
      llvm_unreachable("reset groups cannot commit as one action");
    case SimTickActionKind::Reg:
      os << "    " << regInstanceName(id) << "->tick_commit();\n";
      break;
    case SimTickActionKind::Fifo:
    case SimTickActionKind::AsyncFifo:
      os << "    " << fifoInstanceName(id) << ".tick_commit();\n";
      break;
    case SimTickActionKind::ByteMem:
      os << "    " << memoryInstanceName(id) << ".tick_commit();\n";
      break;
    case SimTickActionKind::SyncMem:
    case SimTickActionKind::SyncMemDP:
      os << "    " << memoryInstanceName(id) << "->tick_commit();\n";
      break;
    case SimTickActionKind::CdcSync:
      os << "    " << nt.get(plan.graph.topNodes[id].outputIds.front())
         << "_inst.tick_commit();\n";
      break;
    }
  }
  if (!instInfos.empty()) {
    os << "    // Force re-eval on next eval() only for stateful sub-modules.\n";
    for (unsigned i = 0; i < instInfos.size(); ++i) {
      if (plan.invalidateCachesOnCommit.contains(instInfos[i].nodeId)) {
        os << "    " << instInfos[i].member << "_eval_cache_valid = false;\n";
      }
    }
  }
  for (unsigned id : fifos)
    if (plan.invalidateCachesOnCommit.contains(id))
      os << "    " << fifoInstanceName(id) << "_eval_cache_valid = false;\n";
  for (unsigned id : byteMems)
    if (plan.invalidateCachesOnCommit.contains(id))
      os << "    " << memoryInstanceName(id) << "_eval_cache_valid = false;\n";
	  for (unsigned id : asyncFifos)
	    if (plan.invalidateCachesOnCommit.contains(id))
	      os << "    " << fifoInstanceName(id) << "_eval_cache_valid = false;\n";
	  os << "  }\n\n";

	  // Decision 0027: provide explicit comb/tick/commit APIs + high-level step().
	  // Decision 0001: expose transfer() as an alias of commit().
	  os << "  void comb() { eval(); }\n";
	  os << "  void tick() { tick_compute(); }\n";
	  os << "  void commit() { tick_commit(); }\n";
	  os << "  void transfer() { tick_commit(); }\n";
	  os << "  void step() {\n";
	  for (SimulationPhase phase : plan.stepPhases) {
	    switch (phase) {
	    case SimulationPhase::Comb: os << "    comb();\n"; break;
	    case SimulationPhase::TickCompute: os << "    tick();\n"; break;
	    case SimulationPhase::TickCommit: os << "    commit();\n"; break;
	    }
	  }
	  os << "  }\n";

	  os << "};\n\n";
	  return success();
	}

} // namespace

LogicalResult emitCpp(const ModuleSimulationPlan &plan, llvm::raw_ostream &os, const CppEmitterOptions &opts) {
  os << "// pyCircuit C++ emission (prototype)\n";
  os << "#include <cstdlib>\n";
  os << "#include <cstdint>\n";
  os << "#include <fstream>\n";
  os << "#include <iostream>\n";
  os << "#include <memory>\n";
  os << "#include <string>\n";
  os << "#include <cpp/pyc_sim.hpp>\n\n";
  os << "namespace pyc::gen {\n\n";

  for (const SimulationPlan &function : plan.functions) {
    if (failed(emitFunc(function, os, opts)))
      return failure();
  }

  os << "} // namespace pyc::gen\n";
  return success();
}

LogicalResult emitCppFunc(const SimulationPlan &plan, llvm::raw_ostream &os, const CppEmitterOptions &opts) {
  return emitFunc(plan, os, opts);
}

} // namespace pyc
