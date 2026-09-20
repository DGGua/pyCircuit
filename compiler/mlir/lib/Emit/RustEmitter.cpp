#include "pyc/Emit/RustEmitter.h"

#include "pyc/Dialect/PYC/PYCOps.h"
#include "pyc/Dialect/PYC/PYCTypes.h"

#include "mlir/Dialect/Arith/IR/Arith.h"
#include "mlir/Dialect/Func/IR/FuncOps.h"
#include "mlir/IR/BuiltinOps.h"
#include "mlir/IR/Value.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallSet.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/ADT/StringSet.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

using namespace mlir;

namespace pyc {
namespace {

static bool isRustKeyword(llvm::StringRef s) {
  static const llvm::StringSet<> kws = {
      "as",       "async",   "await",  "break",  "const",   "continue", "crate",
      "dyn",      "else",    "enum",   "extern", "false",   "fn",       "for",
      "if",       "impl",    "in",     "let",    "loop",    "match",    "mod",
      "move",     "mut",     "pub",    "ref",    "return",  "self",     "Self",
      "static",   "struct",  "super",  "trait",  "true",    "type",     "unsafe",
      "use",      "where",   "while",  "union",  "try",     "gen",      "abstract",
      "become",   "box",     "do",     "final",  "macro",   "override", "priv",
      "typeof",   "unsized", "virtual","yield",
  };
  return kws.contains(s);
}

static std::string sanitizeId(llvm::StringRef s) {
  std::string out;
  out.reserve(s.size() + 2);
  auto isAlpha = [](char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); };
  auto isDigit = [](char c) { return c >= '0' && c <= '9'; };
  auto isOk = [&](char c) { return isAlpha(c) || isDigit(c) || c == '_'; };
  for (char c : s)
    out.push_back(isOk(c) ? c : '_');
  if (out.empty() || isDigit(out.front()))
    out.insert(out.begin(), '_');
  // Collapse "__" so names stay snake_case-friendly (e.g. count__next).
  std::string collapsed;
  collapsed.reserve(out.size());
  for (char c : out) {
    if (c == '_' && !collapsed.empty() && collapsed.back() == '_')
      continue;
    collapsed.push_back(c);
  }
  out = std::move(collapsed);
  if (isRustKeyword(out))
    out = "r_" + out;
  return out;
}

static std::string rustTypeName(llvm::StringRef s) {
  std::string id = sanitizeId(s);
  std::string out;
  bool cap = true;
  for (char c : id) {
    if (c == '_') {
      cap = true;
      continue;
    }
    out.push_back(cap ? static_cast<char>(std::toupper(static_cast<unsigned char>(c))) : c);
    cap = false;
  }
  if (out.empty())
    out = "Module";
  if (isRustKeyword(out))
    out = "R" + out;
  return out;
}

static unsigned bitWidth(Type ty) {
  if (isa<pyc::ClockType>(ty) || isa<pyc::ResetType>(ty))
    return 1;
  if (auto intTy = dyn_cast<IntegerType>(ty))
    return intTy.getWidth();
  return 0;
}

static LogicalResult checkScalarWidth(Operation *op, Type ty, const char *what) {
  if (isa<VectorType>(ty))
    return op->emitError("Rust emitter v1 subset does not support vector types (")
           << what << ")";
  unsigned w = bitWidth(ty);
  if (w == 0 || w > 64)
    return op->emitError("Rust emitter v1 subset only supports scalar widths 1..=64 (")
           << what << ")";
  return success();
}

static std::string rustPrimTy(unsigned w) {
  if (w <= 1)
    return "bool";
  if (w <= 8)
    return "u8";
  if (w <= 16)
    return "u16";
  if (w <= 32)
    return "u32";
  return "u64";
}

static unsigned rustStorageBits(unsigned w) {
  if (w <= 1)
    return 1;
  if (w <= 8)
    return 8;
  if (w <= 16)
    return 16;
  if (w <= 32)
    return 32;
  return 64;
}

static bool rustNeedsMask(unsigned w) { return w > 1 && w != rustStorageBits(w); }

static std::string rustType(Type ty) { return rustPrimTy(bitWidth(ty) == 0 ? 1 : bitWidth(ty)); }

static std::string rustZero(unsigned w) { return w <= 1 ? "false" : "0"; }

static std::string rustMaskLit(unsigned w) {
  uint64_t m = w >= 64 ? ~uint64_t{0} : (w == 0 ? 0 : ((uint64_t{1} << w) - 1));
  std::string out = "0x";
  out += llvm::utohexstr(m);
  if (w <= 8)
    out += "u8";
  else if (w <= 16)
    out += "u16";
  else if (w <= 32)
    out += "u32";
  else
    out += "u64";
  return out;
}

static std::string rustLit(unsigned w, uint64_t v) {
  if (w <= 1)
    return (v & 1) ? "true" : "false";
  if (w < 64)
    v &= (uint64_t{1} << w) - 1;
  std::string out = "0x";
  out += llvm::utohexstr(v);
  out += rustPrimTy(w) == "u8" ? "u8" : rustPrimTy(w) == "u16" ? "u16" : rustPrimTy(w) == "u32" ? "u32" : "u64";
  return out;
}

/// Truncate `expr` (already the dest primitive type) if width < storage.
static std::string rustAndMask(const std::string &expr, unsigned w) {
  if (w <= 1 || !rustNeedsMask(w))
    return expr;
  return "(" + expr + ") & " + rustMaskLit(w);
}

static std::string rustAsU64(const std::string &expr, unsigned w) {
  if (w <= 1)
    return "u64::from(" + expr + ")";
  return expr + " as u64";
}

static std::string rustFromU64(const std::string &expr, unsigned w) {
  if (w <= 1)
    return "((" + expr + ") & 1) != 0";
  std::string cast = "(" + expr + ") as " + rustPrimTy(w);
  return rustAndMask(cast, w);
}

static std::string rustAsU32(const std::string &expr, unsigned w) {
  if (w <= 1)
    return "u32::from(" + expr + ")";
  return expr + " as u32";
}

static std::string rustAsBool(const std::string &expr, unsigned w) {
  if (w <= 1)
    return expr;
  return "(" + expr + " != 0)";
}

/// C++ `pyc::cpp::shl`: amount >= width yields 0 (not Rust wrapping_shl wrap).
static std::string rustShl(const std::string &val, unsigned w, const std::string &amtU32) {
  if (w <= 1)
    return "if " + amtU32 + " == 0 { " + val + " } else { false }";
  std::string shifted = rustAndMask(val + ".wrapping_shl(_s)", w);
  return "{ let _s = " + amtU32 + "; if _s >= " + std::to_string(w) + " { " + rustZero(w) +
         " } else { " + shifted + " } }";
}

static std::string rustLshr(const std::string &val, unsigned w, const std::string &amtU32) {
  if (w <= 1)
    return "if " + amtU32 + " == 0 { " + val + " } else { false }";
  std::string shifted = rustAndMask(val + ".wrapping_shr(_s)", w);
  return "{ let _s = " + amtU32 + "; if _s >= " + std::to_string(w) + " { " + rustZero(w) +
         " } else { " + shifted + " } }";
}

static std::string rustShlImm(const std::string &val, unsigned w, unsigned amt) {
  if (amt == 0)
    return val;
  if (amt >= w)
    return rustZero(w);
  if (w <= 1)
    return "false";
  return rustAndMask(val + ".wrapping_shl(" + std::to_string(amt) + ")", w);
}

static std::string rustLshrImm(const std::string &val, unsigned w, unsigned amt) {
  if (amt == 0)
    return val;
  if (amt >= w)
    return rustZero(w);
  if (w <= 1)
    return "false";
  return rustAndMask(val + ".wrapping_shr(" + std::to_string(amt) + ")", w);
}

static std::string rustStringLiteral(llvm::StringRef s) {
  std::string out;
  out.reserve(s.size() + 2);
  out.push_back('"');
  for (char c : s) {
    switch (c) {
    case '\\':
      out += "\\\\";
      break;
    case '"':
      out += "\\\"";
      break;
    case '\n':
      out += "\\n";
      break;
    case '\r':
      out += "\\r";
      break;
    case '\t':
      out += "\\t";
      break;
    default:
      out.push_back(c);
      break;
    }
  }
  out.push_back('"');
  return out;
}

struct NameTable {
  llvm::DenseMap<Value, std::string> names;
  llvm::StringMap<unsigned> used;
  int next = 0;

  std::string unique(std::string base) {
    unsigned &n = used[base];
    n++;
    if (n == 1)
      return base;
    return base + "_" + std::to_string(n);
  }

  std::string get(Value v) {
    if (auto it = names.find(v); it != names.end())
      return it->second;
    if (Operation *def = v.getDefiningOp()) {
      if (auto nAttr = def->getAttrOfType<StringAttr>("pyc.name")) {
        std::string cand = unique(sanitizeId(nAttr.getValue()));
        names.try_emplace(v, cand);
        return cand;
      }
      std::string base = sanitizeId(def->getName().getStringRef());
      if (base.empty())
        base = "v";
      base += "_" + std::to_string(++next);
      std::string cand = unique(base);
      names.try_emplace(v, cand);
      return cand;
    }
    std::string n = unique("arg_" + std::to_string(++next));
    names.try_emplace(v, n);
    return n;
  }
};

static std::string field(NameTable &nt, Value v) { return "self." + nt.get(v); }

static std::string getPortName(func::FuncOp f, unsigned idx, bool isResult) {
  if (!isResult) {
    if (auto names = f->getAttrOfType<ArrayAttr>("arg_names")) {
      if (idx < names.size())
        if (auto s = dyn_cast<StringAttr>(names[idx]))
          return sanitizeId(s.getValue());
    }
    return "arg" + std::to_string(idx);
  }
  if (auto names = f->getAttrOfType<ArrayAttr>("result_names")) {
    if (idx < names.size())
      if (auto s = dyn_cast<StringAttr>(names[idx]))
        return sanitizeId(s.getValue());
  }
  return "out" + std::to_string(idx);
}

static void computeUniquePortNames(func::FuncOp f, std::vector<std::string> &inNames,
                                   std::vector<std::string> &outNames) {
  NameTable nt;
  inNames.clear();
  outNames.clear();
  for (unsigned i = 0; i < f.getNumArguments(); ++i)
    inNames.push_back(nt.unique(getPortName(f, i, /*isResult=*/false)));
  for (unsigned i = 0; i < f.getNumResults(); ++i)
    outNames.push_back(nt.unique(getPortName(f, i, /*isResult=*/true)));
}

static LogicalResult unsupportedOp(Operation &op) {
  return op.emitError("unsupported op for Rust emitter v1 subset: ") << op.getName();
}

static void assignLine(llvm::raw_ostream &os, NameTable &nt, Value result,
                       llvm::function_ref<void(llvm::raw_ostream &)> buildExpr) {
  os << "        " << field(nt, result) << " = ";
  buildExpr(os);
  os << ";\n";
}

static LogicalResult emitCombAssign(Operation &op, llvm::raw_ostream &os, NameTable &nt) {
  if (auto c = dyn_cast<pyc::ConstantOp>(op)) {
    if (failed(checkScalarWidth(&op, c.getType(), "pyc.constant")))
      return failure();
    unsigned w = bitWidth(c.getType());
    auto v = c.getValueAttr().getValue();
    uint64_t word = v.getRawData()[0];
    if (w < 64)
      word &= (1ull << w) - 1ull;
    assignLine(os, nt, c.getResult(), [&](llvm::raw_ostream &e) { e << rustLit(w, word); });
    return success();
  }
  if (auto a = dyn_cast<pyc::AliasOp>(op)) {
    if (failed(checkScalarWidth(&op, a.getType(), "pyc.alias")))
      return failure();
    assignLine(os, nt, a.getResult(),
               [&](llvm::raw_ostream &e) { e << field(nt, a.getIn()); });
    return success();
  }
  if (auto ra = dyn_cast<pyc::ResetActiveOp>(op)) {
    assignLine(os, nt, ra.getActive(),
               [&](llvm::raw_ostream &e) { e << field(nt, ra.getRst()); });
    return success();
  }
  if (auto a = dyn_cast<pyc::AddOp>(op)) {
    if (failed(checkScalarWidth(&op, a.getType(), "pyc.add")))
      return failure();
    unsigned w = bitWidth(a.getType());
    assignLine(os, nt, a.getResult(), [&](llvm::raw_ostream &e) {
      if (w <= 1) {
        e << field(nt, a.getLhs()) << " ^ " << field(nt, a.getRhs());
        return;
      }
      e << rustAndMask(field(nt, a.getLhs()) + ".wrapping_add(" + field(nt, a.getRhs()) + ")", w);
    });
    return success();
  }
  if (auto s = dyn_cast<pyc::SubOp>(op)) {
    if (failed(checkScalarWidth(&op, s.getType(), "pyc.sub")))
      return failure();
    unsigned w = bitWidth(s.getType());
    assignLine(os, nt, s.getResult(), [&](llvm::raw_ostream &e) {
      if (w <= 1) {
        e << field(nt, s.getLhs()) << " ^ " << field(nt, s.getRhs());
        return;
      }
      e << rustAndMask(field(nt, s.getLhs()) + ".wrapping_sub(" + field(nt, s.getRhs()) + ")", w);
    });
    return success();
  }
  if (auto m = dyn_cast<pyc::MulOp>(op)) {
    if (failed(checkScalarWidth(&op, m.getType(), "pyc.mul")))
      return failure();
    unsigned w = bitWidth(m.getType());
    assignLine(os, nt, m.getResult(), [&](llvm::raw_ostream &e) {
      if (w <= 1) {
        e << field(nt, m.getLhs()) << " & " << field(nt, m.getRhs());
        return;
      }
      e << rustAndMask(field(nt, m.getLhs()) + ".wrapping_mul(" + field(nt, m.getRhs()) + ")", w);
    });
    return success();
  }
  if (auto d = dyn_cast<pyc::UdivOp>(op)) {
    unsigned w = bitWidth(d.getResult().getType());
    if (failed(checkScalarWidth(&op, d.getType(), "pyc.udiv")))
      return failure();
    assignLine(os, nt, d.getResult(), [&](llvm::raw_ostream &e) {
      e << rustFromU64("udiv_bits(" + rustAsU64(field(nt, d.getLhs()), w) + ", " +
                           rustAsU64(field(nt, d.getRhs()), w) + ", " + std::to_string(w) + ")",
                       w);
    });
    return success();
  }
  if (auto r = dyn_cast<pyc::UremOp>(op)) {
    unsigned w = bitWidth(r.getResult().getType());
    if (failed(checkScalarWidth(&op, r.getType(), "pyc.urem")))
      return failure();
    assignLine(os, nt, r.getResult(), [&](llvm::raw_ostream &e) {
      e << rustFromU64("urem_bits(" + rustAsU64(field(nt, r.getLhs()), w) + ", " +
                           rustAsU64(field(nt, r.getRhs()), w) + ", " + std::to_string(w) + ")",
                       w);
    });
    return success();
  }
  if (auto d = dyn_cast<pyc::SdivOp>(op)) {
    unsigned w = bitWidth(d.getResult().getType());
    if (failed(checkScalarWidth(&op, d.getType(), "pyc.sdiv")))
      return failure();
    assignLine(os, nt, d.getResult(), [&](llvm::raw_ostream &e) {
      e << rustFromU64("sdiv_bits(" + rustAsU64(field(nt, d.getLhs()), w) + ", " +
                           rustAsU64(field(nt, d.getRhs()), w) + ", " + std::to_string(w) + ")",
                       w);
    });
    return success();
  }
  if (auto r = dyn_cast<pyc::SremOp>(op)) {
    unsigned w = bitWidth(r.getResult().getType());
    if (failed(checkScalarWidth(&op, r.getType(), "pyc.srem")))
      return failure();
    assignLine(os, nt, r.getResult(), [&](llvm::raw_ostream &e) {
      e << rustFromU64("srem_bits(" + rustAsU64(field(nt, r.getLhs()), w) + ", " +
                           rustAsU64(field(nt, r.getRhs()), w) + ", " + std::to_string(w) + ")",
                       w);
    });
    return success();
  }
  if (auto m = dyn_cast<pyc::MuxOp>(op)) {
    if (failed(checkScalarWidth(&op, m.getType(), "pyc.mux")))
      return failure();
    if (isa<VectorType>(m.getSel().getType()))
      return m.emitError("Rust emitter v1 subset does not support vector mux select");
    assignLine(os, nt, m.getResult(), [&](llvm::raw_ostream &e) {
      e << "if " << field(nt, m.getSel()) << " { " << field(nt, m.getA()) << " } else { "
        << field(nt, m.getB()) << " }";
    });
    return success();
  }
  if (auto s = dyn_cast<arith::SelectOp>(op)) {
    if (!s.getCondition().getType().isInteger(1))
      return s.emitError("Rust emitter only supports arith.select with i1 condition");
    if (failed(checkScalarWidth(&op, s.getType(), "arith.select")))
      return failure();
    assignLine(os, nt, s.getResult(), [&](llvm::raw_ostream &e) {
      e << "if " << field(nt, s.getCondition()) << " { " << field(nt, s.getTrueValue()) << " } else { "
        << field(nt, s.getFalseValue()) << " }";
    });
    return success();
  }
  if (auto a = dyn_cast<pyc::AndOp>(op)) {
    if (failed(checkScalarWidth(&op, a.getType(), "pyc.and")))
      return failure();
    assignLine(os, nt, a.getResult(), [&](llvm::raw_ostream &e) {
      e << field(nt, a.getLhs()) << " & " << field(nt, a.getRhs());
    });
    return success();
  }
  if (auto o = dyn_cast<pyc::OrOp>(op)) {
    if (failed(checkScalarWidth(&op, o.getType(), "pyc.or")))
      return failure();
    assignLine(os, nt, o.getResult(), [&](llvm::raw_ostream &e) {
      e << field(nt, o.getLhs()) << " | " << field(nt, o.getRhs());
    });
    return success();
  }
  if (auto x = dyn_cast<pyc::XorOp>(op)) {
    if (failed(checkScalarWidth(&op, x.getType(), "pyc.xor")))
      return failure();
    assignLine(os, nt, x.getResult(), [&](llvm::raw_ostream &e) {
      e << field(nt, x.getLhs()) << " ^ " << field(nt, x.getRhs());
    });
    return success();
  }
  if (auto n = dyn_cast<pyc::NotOp>(op)) {
    if (failed(checkScalarWidth(&op, n.getType(), "pyc.not")))
      return failure();
    unsigned w = bitWidth(n.getType());
    assignLine(os, nt, n.getResult(), [&](llvm::raw_ostream &e) {
      if (w <= 1)
        e << "!" << field(nt, n.getIn());
      else
        e << rustAndMask("!" + field(nt, n.getIn()), w);
    });
    return success();
  }
  if (auto e = dyn_cast<pyc::EqOp>(op)) {
    if (failed(checkScalarWidth(&op, e.getLhs().getType(), "pyc.eq")))
      return failure();
    assignLine(os, nt, e.getResult(), [&](llvm::raw_ostream &eout) {
      eout << field(nt, e.getLhs()) << " == " << field(nt, e.getRhs());
    });
    return success();
  }
  if (auto u = dyn_cast<pyc::UltOp>(op)) {
    if (failed(checkScalarWidth(&op, u.getLhs().getType(), "pyc.ult")))
      return failure();
    assignLine(os, nt, u.getResult(), [&](llvm::raw_ostream &eout) {
      eout << field(nt, u.getLhs()) << " < " << field(nt, u.getRhs());
    });
    return success();
  }
  if (auto s = dyn_cast<pyc::SltOp>(op)) {
    unsigned w = bitWidth(s.getLhs().getType());
    if (failed(checkScalarWidth(&op, s.getLhs().getType(), "pyc.slt")))
      return failure();
    assignLine(os, nt, s.getResult(), [&](llvm::raw_ostream &eout) {
      eout << "slt_bits(" << rustAsU64(field(nt, s.getLhs()), w) << ", "
           << rustAsU64(field(nt, s.getRhs()), w) << ", " << w << ")";
    });
    return success();
  }
  if (auto t = dyn_cast<pyc::TruncOp>(op)) {
    unsigned iw = bitWidth(t.getIn().getType());
    unsigned ow = bitWidth(t.getResult().getType());
    if (failed(checkScalarWidth(&op, t.getIn().getType(), "pyc.trunc in")) ||
        failed(checkScalarWidth(&op, t.getType(), "pyc.trunc out")))
      return failure();
    assignLine(os, nt, t.getResult(), [&](llvm::raw_ostream &e) {
      e << rustFromU64(rustAsU64(field(nt, t.getIn()), iw), ow);
    });
    return success();
  }
  if (auto z = dyn_cast<pyc::ZextOp>(op)) {
    unsigned iw = bitWidth(z.getIn().getType());
    unsigned ow = bitWidth(z.getResult().getType());
    if (failed(checkScalarWidth(&op, z.getIn().getType(), "pyc.zext in")) ||
        failed(checkScalarWidth(&op, z.getType(), "pyc.zext out")))
      return failure();
    assignLine(os, nt, z.getResult(), [&](llvm::raw_ostream &e) {
      e << rustFromU64(rustAsU64(field(nt, z.getIn()), iw), ow);
    });
    return success();
  }
  if (auto s = dyn_cast<pyc::SextOp>(op)) {
    unsigned iw = bitWidth(s.getIn().getType());
    unsigned ow = bitWidth(s.getResult().getType());
    if (failed(checkScalarWidth(&op, s.getIn().getType(), "pyc.sext in")) ||
        failed(checkScalarWidth(&op, s.getType(), "pyc.sext out")))
      return failure();
    assignLine(os, nt, s.getResult(), [&](llvm::raw_ostream &e) {
      e << rustFromU64("sext_bits(" + rustAsU64(field(nt, s.getIn()), iw) + ", " +
                           std::to_string(iw) + ", " + std::to_string(ow) + ")",
                       ow);
    });
    return success();
  }
  if (auto ex = dyn_cast<pyc::ExtractOp>(op)) {
    unsigned iw = bitWidth(ex.getIn().getType());
    unsigned ow = bitWidth(ex.getResult().getType());
    if (failed(checkScalarWidth(&op, ex.getIn().getType(), "pyc.extract in")) ||
        failed(checkScalarWidth(&op, ex.getType(), "pyc.extract out")))
      return failure();
    unsigned lsb = static_cast<unsigned>(ex.getLsbAttr().getInt());
    assignLine(os, nt, ex.getResult(), [&](llvm::raw_ostream &e) {
      e << rustFromU64(rustAsU64(field(nt, ex.getIn()), iw) + " >> " + std::to_string(lsb), ow);
    });
    return success();
  }
  if (auto sh = dyn_cast<pyc::ShliOp>(op)) {
    unsigned w = bitWidth(sh.getResult().getType());
    if (failed(checkScalarWidth(&op, sh.getType(), "pyc.shli")))
      return failure();
    unsigned amt = static_cast<unsigned>(sh.getAmountAttr().getInt());
    assignLine(os, nt, sh.getResult(), [&](llvm::raw_ostream &e) {
      e << rustShlImm(field(nt, sh.getIn()), w, amt);
    });
    return success();
  }
  if (auto sh = dyn_cast<pyc::LshriOp>(op)) {
    unsigned w = bitWidth(sh.getResult().getType());
    if (failed(checkScalarWidth(&op, sh.getType(), "pyc.lshri")))
      return failure();
    unsigned amt = static_cast<unsigned>(sh.getAmountAttr().getInt());
    assignLine(os, nt, sh.getResult(), [&](llvm::raw_ostream &e) {
      e << rustLshrImm(field(nt, sh.getIn()), w, amt);
    });
    return success();
  }
  if (auto sh = dyn_cast<pyc::AshriOp>(op)) {
    unsigned w = bitWidth(sh.getResult().getType());
    if (failed(checkScalarWidth(&op, sh.getType(), "pyc.ashri")))
      return failure();
    unsigned amt = static_cast<unsigned>(sh.getAmountAttr().getInt());
    assignLine(os, nt, sh.getResult(), [&](llvm::raw_ostream &e) {
      e << rustFromU64("ashr_bits(" + rustAsU64(field(nt, sh.getIn()), w) + ", " +
                           std::to_string(w) + ", " + std::to_string(amt) + ")",
                       w);
    });
    return success();
  }
  if (auto sh = dyn_cast<pyc::ShlOp>(op)) {
    unsigned w = bitWidth(sh.getResult().getType());
    if (failed(checkScalarWidth(&op, sh.getType(), "pyc.shl")))
      return failure();
    unsigned aw = bitWidth(sh.getAmount().getType());
    assignLine(os, nt, sh.getResult(), [&](llvm::raw_ostream &e) {
      e << rustShl(field(nt, sh.getIn()), w, rustAsU32(field(nt, sh.getAmount()), aw));
    });
    return success();
  }
  if (auto sh = dyn_cast<pyc::LshrOp>(op)) {
    unsigned w = bitWidth(sh.getResult().getType());
    if (failed(checkScalarWidth(&op, sh.getType(), "pyc.lshr")))
      return failure();
    unsigned aw = bitWidth(sh.getAmount().getType());
    assignLine(os, nt, sh.getResult(), [&](llvm::raw_ostream &e) {
      e << rustLshr(field(nt, sh.getIn()), w, rustAsU32(field(nt, sh.getAmount()), aw));
    });
    return success();
  }
  if (auto sh = dyn_cast<pyc::AshrOp>(op)) {
    unsigned w = bitWidth(sh.getResult().getType());
    if (failed(checkScalarWidth(&op, sh.getType(), "pyc.ashr")))
      return failure();
    unsigned aw = bitWidth(sh.getAmount().getType());
    assignLine(os, nt, sh.getResult(), [&](llvm::raw_ostream &e) {
      e << rustFromU64("ashr_bits(" + rustAsU64(field(nt, sh.getIn()), w) + ", " +
                           std::to_string(w) + ", " + rustAsU32(field(nt, sh.getAmount()), aw) +
                           ")",
                       w);
    });
    return success();
  }
  if (auto c = dyn_cast<pyc::ConcatOp>(op)) {
    if (failed(checkScalarWidth(&op, c.getType(), "pyc.concat")))
      return failure();
    if (c.getInputs().empty())
      return c.emitError("pyc.concat requires at least one input");
    for (Value in : c.getInputs()) {
      if (failed(checkScalarWidth(&op, in.getType(), "pyc.concat input")))
        return failure();
    }
    unsigned ow = bitWidth(c.getType());
    assignLine(os, nt, c.getResult(), [&](llvm::raw_ostream &e) {
      // Nest from the right so MSB-first concat matches C++ concat(a, concat(b, ...)).
      std::string acc;
      std::function<void(unsigned)> emitFrom = [&](unsigned i) {
        Value in = c.getInputs()[i];
        unsigned aw = bitWidth(in.getType());
        std::string piece = rustAsU64(field(nt, in), aw);
        if (i + 1 == c.getNumOperands()) {
          acc = piece;
          return;
        }
        unsigned restW = 0;
        for (unsigned j = i + 1; j < c.getNumOperands(); ++j)
          restW += bitWidth(c.getInputs()[j].getType());
        emitFrom(i + 1);
        acc = "((" + piece + " << " + std::to_string(restW) + ") | " + acc + ")";
      };
      emitFrom(0);
      e << rustFromU64(acc, ow);
    });
    return success();
  }
  return unsupportedOp(op);
}

static LogicalResult emitEvalNode(Operation &op, llvm::raw_ostream &os, NameTable &nt,
                                  const llvm::DenseMap<Operation *, std::string> &instMember,
                                  const llvm::DenseMap<Operation *, std::vector<std::string>> &instInPorts,
                                  const llvm::DenseMap<Operation *, std::vector<std::string>> &instOutPorts,
                                  const llvm::DenseMap<Operation *, unsigned> &combIndex) {
  if (isa<func::ReturnOp>(op) || isa<pyc::WireOp>(op) || isa<pyc::RegOp>(op))
    return success();
  if (auto a = dyn_cast<pyc::AssignOp>(op)) {
    if (failed(checkScalarWidth(&op, a.getDst().getType(), "pyc.assign")))
      return failure();
    os << "        " << field(nt, a.getDst()) << " = " << field(nt, a.getSrc()) << ";\n";
    return success();
  }
  if (auto a = dyn_cast<pyc::AssertOp>(op)) {
    std::string msg = "pyc.assert failed";
    if (auto m = a.getMsgAttr())
      msg = m.getValue().str();
    os << "        if !" << rustAsBool(field(nt, a.getCond()), bitWidth(a.getCond().getType()))
       << " { panic!(\"{}\", " << rustStringLiteral(msg) << "); }\n";
    return success();
  }
  if (auto comb = dyn_cast<pyc::CombOp>(op)) {
    os << "        self.eval_comb_" << combIndex.lookup(comb.getOperation()) << "();\n";
    return success();
  }
  if (auto inst = dyn_cast<pyc::InstanceOp>(op)) {
    auto mit = instMember.find(inst.getOperation());
    if (mit == instMember.end())
      return inst.emitError("internal error: missing instance metadata");
    const std::string &member = mit->second;
    const auto &ins = instInPorts.lookup(inst.getOperation());
    const auto &outs = instOutPorts.lookup(inst.getOperation());
    if (ins.size() != inst.getNumOperands() || outs.size() != inst.getNumResults())
      return inst.emitError("instance port count mismatch");
    for (unsigned i = 0; i < inst.getNumOperands(); ++i)
      os << "        self." << member << "." << ins[i] << " = " << field(nt, inst.getOperand(i))
         << ";\n";
    os << "        self." << member << ".eval();\n";
    for (unsigned i = 0; i < inst.getNumResults(); ++i)
      os << "        " << field(nt, inst.getResult(i)) << " = self." << member << "." << outs[i]
         << ";\n";
    return success();
  }
  if (isa<pyc::FifoOp, pyc::ByteMemOp, pyc::SyncMemOp, pyc::SyncMemDPOp, pyc::AsyncFifoOp,
          pyc::CdcSyncOp>(op))
    return op.emitError("Rust emitter v1 subset does not support memory/FIFO/CDC primitives");
  return emitCombAssign(op, os, nt);
}

static LogicalResult emitCombMethod(pyc::CombOp comb, llvm::raw_ostream &os, NameTable &nt,
                                   unsigned idx) {
  if (comb.getBody().empty())
    return comb.emitError("pyc.comb must have a non-empty region");
  Block &b = comb.getBody().front();
  if (b.getNumArguments() != comb.getNumOperands())
    return comb.emitError("pyc.comb body block argument count must match inputs");
  for (auto [i, arg] : llvm::enumerate(b.getArguments()))
    nt.names.try_emplace(arg, nt.get(comb.getInputs()[i]));

  os << "    fn eval_comb_" << idx << "(&mut self) {\n";
  for (Operation &op : b) {
    if (isa<pyc::YieldOp>(op))
      break;
    if (failed(emitCombAssign(op, os, nt)))
      return failure();
  }
  if (auto yield = dyn_cast<pyc::YieldOp>(b.getTerminator())) {
    if (yield.getNumOperands() != comb.getNumResults())
      return comb.emitError("pyc.comb yield arity mismatch");
    for (auto [i, v] : llvm::enumerate(yield.getOperands()))
      os << "        " << field(nt, comb.getResult(i)) << " = " << field(nt, v) << ";\n";
  }
  os << "    }\n\n";
  return success();
}

static LogicalResult emitFunc(func::FuncOp f, llvm::raw_ostream &os) {
  NameTable nt;
  if (!llvm::hasSingleElement(f.getBody()))
    return f.emitError("Rust emitter currently supports single-block functions only");

  Block &top = f.getBody().front();
  std::string structName = rustTypeName(f.getSymName());

  std::vector<std::string> inNames;
  std::vector<std::string> outNames;
  inNames.reserve(f.getNumArguments());
  outNames.reserve(f.getNumResults());
  for (auto [i, arg] : llvm::enumerate(f.getArguments())) {
    if (failed(checkScalarWidth(f.getOperation(), arg.getType(), "input port")))
      return failure();
    std::string name = nt.unique(getPortName(f, i, /*isResult=*/false));
    inNames.push_back(name);
    nt.names.try_emplace(arg, name);
  }
  for (unsigned i = 0; i < f.getNumResults(); ++i) {
    if (failed(checkScalarWidth(f.getOperation(), f.getResultTypes()[i], "output port")))
      return failure();
    outNames.push_back(nt.unique(getPortName(f, i, /*isResult=*/true)));
  }

  llvm::SmallVector<pyc::RegOp> regs;
  llvm::SmallVector<pyc::InstanceOp> instances;
  llvm::SmallVector<pyc::CombOp> combs;
  for (Operation &op : top) {
    if (auto r = dyn_cast<pyc::RegOp>(op)) {
      if (failed(checkScalarWidth(&op, r.getQ().getType(), "pyc.reg")))
        return failure();
      regs.push_back(r);
    } else if (auto inst = dyn_cast<pyc::InstanceOp>(op)) {
      instances.push_back(inst);
    } else if (auto comb = dyn_cast<pyc::CombOp>(op)) {
      combs.push_back(comb);
    } else if (isa<pyc::FifoOp, pyc::ByteMemOp, pyc::SyncMemOp, pyc::SyncMemDPOp, pyc::AsyncFifoOp,
                   pyc::CdcSyncOp>(op)) {
      return op.emitError("Rust emitter v1 subset does not support memory/FIFO/CDC primitives");
    }
  }

  ModuleOp mod = f->getParentOfType<ModuleOp>();
  if (!mod)
    return f.emitError("Rust emitter: missing parent module for instance resolution");

  struct InstInfo {
    pyc::InstanceOp op;
    func::FuncOp callee;
    std::string member;
    std::string rustTy;
    std::vector<std::string> inPorts;
    std::vector<std::string> outPorts;
  };
  std::vector<InstInfo> instInfos;
  llvm::DenseMap<Operation *, std::string> instMember;
  llvm::DenseMap<Operation *, std::vector<std::string>> instInPorts;
  llvm::DenseMap<Operation *, std::vector<std::string>> instOutPorts;

  for (auto inst : instances) {
    auto calleeAttr = inst->getAttrOfType<FlatSymbolRefAttr>("callee");
    if (!calleeAttr)
      return inst.emitError("missing required FlatSymbolRefAttr `callee`");
    auto callee = mod.lookupSymbol<func::FuncOp>(calleeAttr.getValue());
    if (!callee)
      return inst.emitError("callee symbol not found: ") << calleeAttr.getValue();
    std::vector<std::string> inPorts;
    std::vector<std::string> outPorts;
    computeUniquePortNames(callee, inPorts, outPorts);
    if (inPorts.size() != inst.getNumOperands())
      return inst.emitError("operand count does not match callee signature");
    if (outPorts.size() != inst.getNumResults())
      return inst.emitError("result count does not match callee signature");
    for (Value v : inst.getOperands()) {
      if (failed(checkScalarWidth(inst.getOperation(), v.getType(), "instance input")))
        return failure();
    }
    for (Value v : inst.getResults()) {
      if (failed(checkScalarWidth(inst.getOperation(), v.getType(), "instance output")))
        return failure();
    }
    std::string base = "inst";
    if (auto nameAttr = inst->getAttrOfType<StringAttr>("name"))
      base = sanitizeId(nameAttr.getValue());
    else
      base = sanitizeId(callee.getSymName()) + std::string("_inst");
    std::string member = nt.unique(base);
    std::string rustTy = rustTypeName(callee.getSymName());
    instMember.try_emplace(inst.getOperation(), member);
    instInPorts.try_emplace(inst.getOperation(), inPorts);
    instOutPorts.try_emplace(inst.getOperation(), outPorts);
    instInfos.push_back(
        InstInfo{inst, callee, member, rustTy, std::move(inPorts), std::move(outPorts)});
  }

  os << "#[allow(non_camel_case_types, non_snake_case, dead_code)]\n";
  os << "pub struct " << structName << " {\n";
  for (unsigned i = 0; i < inNames.size(); ++i)
    os << "    pub " << inNames[i] << ": " << rustType(f.getArgument(i).getType()) << ",\n";
  for (unsigned i = 0; i < outNames.size(); ++i)
    os << "    pub " << outNames[i] << ": " << rustType(f.getResultTypes()[i]) << ",\n";

  llvm::DenseSet<Value> declared;
  for (Value arg : f.getArguments())
    declared.insert(arg);

  f.walk([&](Operation *op) {
    for (Value r : op->getResults()) {
      if (declared.contains(r))
        continue;
      // Skip types we already rejected; walk still sees them if we returned earlier.
      if (bitWidth(r.getType()) == 0 || isa<VectorType>(r.getType()))
        continue;
      os << "    pub " << nt.get(r) << ": " << rustType(r.getType()) << ",\n";
      declared.insert(r);
    }
  });

  for (auto r : regs) {
    os << "    " << nt.get(r.getQ()) << "_inst: PycReg<" << rustType(r.getQ().getType()) << ">,\n";
  }
  for (const auto &ii : instInfos)
    os << "    pub " << ii.member << ": Box<" << ii.rustTy << ">,\n";
  os << "}\n\n";

  os << "impl Default for " << structName << " {\n";
  os << "    fn default() -> Self {\n";
  os << "        Self {\n";
  for (unsigned i = 0; i < inNames.size(); ++i)
    os << "            " << inNames[i] << ": " << rustZero(bitWidth(f.getArgument(i).getType()))
       << ",\n";
  for (unsigned i = 0; i < outNames.size(); ++i)
    os << "            " << outNames[i] << ": " << rustZero(bitWidth(f.getResultTypes()[i]))
       << ",\n";
  declared.clear();
  for (Value arg : f.getArguments())
    declared.insert(arg);
  f.walk([&](Operation *op) {
    for (Value r : op->getResults()) {
      if (declared.contains(r))
        continue;
      if (bitWidth(r.getType()) == 0 || isa<VectorType>(r.getType()))
        continue;
      os << "            " << nt.get(r) << ": " << rustZero(bitWidth(r.getType())) << ",\n";
      declared.insert(r);
    }
  });
  for (auto r : regs)
    os << "            " << nt.get(r.getQ()) << "_inst: PycReg::new(),\n";
  for (const auto &ii : instInfos)
    os << "            " << ii.member << ": Box::new(" << ii.rustTy << "::default()),\n";
  os << "        }\n";
  os << "    }\n";
  os << "}\n\n";

  os << "impl " << structName << " {\n";
  os << "    pub fn new() -> Self { Self::default() }\n\n";

  llvm::DenseMap<Operation *, unsigned> combIndex;
  for (auto [i, comb] : llvm::enumerate(combs)) {
    combIndex.try_emplace(comb.getOperation(), static_cast<unsigned>(i));
    if (failed(emitCombMethod(comb, os, nt, static_cast<unsigned>(i))))
      return failure();
  }

  os << "    pub fn eval(&mut self) {\n";
  for (Operation &op : top) {
    if (failed(emitEvalNode(op, os, nt, instMember, instInPorts, instOutPorts, combIndex)))
      return failure();
  }
  auto ret = dyn_cast_or_null<func::ReturnOp>(top.getTerminator());
  if (!ret)
    return f.emitError("missing return");
  for (auto [i, v] : llvm::enumerate(ret.getOperands()))
    os << "        self." << outNames[i] << " = " << field(nt, v) << ";\n";
  os << "    }\n\n";

  os << "    pub fn tick_compute(&mut self) {\n";
  for (auto &ii : instInfos) {
    for (unsigned j = 0; j < ii.op.getNumOperands(); ++j)
      os << "        self." << ii.member << "." << ii.inPorts[j] << " = "
         << field(nt, ii.op.getOperand(j)) << ";\n";
    os << "        self." << ii.member << ".tick_compute();\n";
  }
  for (auto r : regs) {
    os << "        self." << nt.get(r.getQ()) << "_inst.tick_compute("
       << rustAsBool(field(nt, r.getClk()), bitWidth(r.getClk().getType())) << ", "
       << rustAsBool(field(nt, r.getRst()), bitWidth(r.getRst().getType())) << ", "
       << rustAsBool(field(nt, r.getEn()), bitWidth(r.getEn().getType())) << ", "
       << field(nt, r.getNext()) << ", " << field(nt, r.getInit()) << ");\n";
  }
  os << "    }\n\n";

  os << "    pub fn tick_commit(&mut self) {\n";
  for (const auto &ii : instInfos)
    os << "        self." << ii.member << ".tick_commit();\n";
  for (auto r : regs) {
    os << "        self." << nt.get(r.getQ()) << "_inst.tick_commit(&mut self." << nt.get(r.getQ())
       << ");\n";
  }
  os << "    }\n\n";

  os << "    pub fn comb(&mut self) { self.eval(); }\n";
  os << "    pub fn tick(&mut self) { self.tick_compute(); }\n";
  os << "    pub fn commit(&mut self) { self.tick_commit(); }\n";
  os << "    pub fn transfer(&mut self) { self.tick_commit(); }\n";
  os << "    pub fn step(&mut self) {\n";
  os << "        self.comb();\n";
  os << "        self.tick();\n";
  os << "        self.transfer();\n";
  os << "        self.comb();\n";
  os << "    }\n";
  os << "}\n\n";
  return success();
}

static LogicalResult topoFuncs(ModuleOp module, llvm::SmallVector<func::FuncOp> &order) {
  llvm::SmallVector<func::FuncOp> funcs;
  for (auto f : module.getOps<func::FuncOp>()) {
    if (!f.isDeclaration())
      funcs.push_back(f);
  }
  llvm::StringMap<unsigned> indexByName;
  for (auto [i, f] : llvm::enumerate(funcs))
    indexByName.try_emplace(f.getSymName(), static_cast<unsigned>(i));

  llvm::SmallVector<llvm::SmallVector<unsigned>> succ(funcs.size());
  llvm::SmallVector<unsigned> indeg(funcs.size(), 0);
  for (auto it : llvm::enumerate(funcs)) {
    unsigned callerIdx = static_cast<unsigned>(it.index());
    it.value().walk([&](pyc::InstanceOp inst) {
      auto calleeAttr = inst->getAttrOfType<FlatSymbolRefAttr>("callee");
      if (!calleeAttr)
        return;
      auto found = indexByName.find(calleeAttr.getValue());
      if (found == indexByName.end())
        return;
      unsigned calleeIdx = found->second;
      succ[calleeIdx].push_back(callerIdx);
      indeg[callerIdx]++;
    });
  }

  auto cmp = [&](unsigned a, unsigned b) { return funcs[a].getSymName() > funcs[b].getSymName(); };
  std::vector<unsigned> heap;
  for (unsigned i = 0; i < funcs.size(); ++i)
    if (indeg[i] == 0)
      heap.push_back(i);
  std::make_heap(heap.begin(), heap.end(), cmp);
  llvm::SmallVector<unsigned> ord;
  while (!heap.empty()) {
    std::pop_heap(heap.begin(), heap.end(), cmp);
    unsigned n = heap.back();
    heap.pop_back();
    ord.push_back(n);
    for (unsigned s : succ[n]) {
      if (--indeg[s] == 0) {
        heap.push_back(s);
        std::push_heap(heap.begin(), heap.end(), cmp);
      }
    }
  }
  if (ord.size() != funcs.size())
    return module.emitError("Rust emitter: module instance graph has a cycle");
  order.clear();
  for (unsigned idx : ord)
    order.push_back(funcs[idx]);
  return success();
}

} // namespace

LogicalResult emitRust(ModuleOp module, llvm::raw_ostream &os) {
  os << "// pyCircuit Rust emission (v1 subset)\n";
  os << "#[allow(non_camel_case_types, non_snake_case, unused_imports, dead_code)]\n";
  os << "use pyc_runtime::*;\n\n";
  llvm::SmallVector<func::FuncOp> order;
  if (failed(topoFuncs(module, order)))
    return failure();
  for (auto f : order) {
    if (failed(emitFunc(f, os)))
      return failure();
  }
  return success();
}

LogicalResult emitRustFunc(ModuleOp module, func::FuncOp f, llvm::raw_ostream &os) {
  (void)module;
  os << "// pyCircuit Rust emission (v1 subset)\n";
  os << "#[allow(non_camel_case_types, non_snake_case, unused_imports, dead_code)]\n";
  os << "use pyc_runtime::*;\n\n";
  return emitFunc(f, os);
}

} // namespace pyc
