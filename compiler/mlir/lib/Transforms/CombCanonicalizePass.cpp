#include "pyc/Transforms/Passes.h"

#include "pyc/Dialect/PYC/PYCOps.h"

#include "mlir/Dialect/Arith/IR/Arith.h"
#include "mlir/Dialect/Func/IR/FuncOps.h"
#include "mlir/IR/PatternMatch.h"
#include "mlir/Pass/Pass.h"
#include "mlir/Transforms/GreedyPatternRewriteDriver.h"

#include <algorithm>
#include <cassert>
#include <map>
#include <type_traits>

using namespace mlir;

namespace pyc {
namespace {

static std::optional<uint64_t> getConstUInt(Value v) {
  if (auto c = v.getDefiningOp<pyc::ConstantOp>())
    return c.getValueAttr().getValue().getZExtValue();
  if (auto c = v.getDefiningOp<arith::ConstantOp>()) {
    if (auto i = dyn_cast<IntegerAttr>(c.getValue()))
      return i.getValue().getZExtValue();
  }
  // Vector constant: v_broadcast of a constant scalar.
  if (auto vb = v.getDefiningOp<pyc::VBroadcastOp>()) {
    if (auto c = vb.getScalar().getDefiningOp<pyc::ConstantOp>())
      return c.getValueAttr().getValue().getZExtValue();
    if (auto c = vb.getScalar().getDefiningOp<arith::ConstantOp>()) {
      if (auto i = dyn_cast<IntegerAttr>(c.getValue()))
        return i.getValue().getZExtValue();
    }
  }
  return std::nullopt;
}

static std::optional<llvm::APInt> getConstAPInt(Value v) {
  if (auto c = v.getDefiningOp<pyc::ConstantOp>())
    return c.getValueAttr().getValue();
  if (auto c = v.getDefiningOp<arith::ConstantOp>())
    if (auto i = dyn_cast<IntegerAttr>(c.getValue()))
      return i.getValue();
  return std::nullopt;
}

static Value stripAlias(Value v) {
  while (auto a = v.getDefiningOp<pyc::AliasOp>())
    v = a.getIn();
  return v;
}

static std::pair<Value, bool> stripNot(Value v) {
  v = stripAlias(v);
  if (auto n = v.getDefiningOp<pyc::NotOp>())
    return {stripAlias(n.getIn()), true};
  return {v, false};
}

static bool andMatches(pyc::AndOp op, Value a, bool aNot, Value b, bool bNot) {
  auto [lBase, lNot] = stripNot(op.getLhs());
  auto [rBase, rNot] = stripNot(op.getRhs());
  return (lBase == a && lNot == aNot && rBase == b && rNot == bNot) ||
         (lBase == b && lNot == bNot && rBase == a && rNot == aNot);
}

// Create a constant value, broadcasting to a vector when needed.
static Value constInt(PatternRewriter &rewriter, Location loc, Type ty, const llvm::APInt &v) {
  // Scalar path.
  if (auto intTy = dyn_cast<IntegerType>(ty)) {
    llvm::APInt vv = v;
    if (vv.getBitWidth() != intTy.getWidth())
      vv = vv.zextOrTrunc(intTy.getWidth());
    return rewriter.create<pyc::ConstantOp>(loc, intTy, IntegerAttr::get(intTy, vv));
  }
  // Vector path: constant + broadcast.
  if (auto vt = dyn_cast<VectorType>(ty)) {
    auto elemTy = dyn_cast<IntegerType>(vt.getElementType());
    if (!elemTy)
      return {};
    llvm::APInt vv = v;
    if (vv.getBitWidth() != elemTy.getWidth())
      vv = vv.zextOrTrunc(elemTy.getWidth());
    Value scalar = rewriter.create<pyc::ConstantOp>(loc, elemTy, IntegerAttr::get(elemTy, vv));
    return rewriter.create<pyc::VBroadcastOp>(loc, vt, scalar, vt.getShape()[0]);
  }
  return {};
}

/// Return the element type for i1 checks: scalar i1 or vector<…xi1>.
static bool isI1OrI1Vector(Type ty) {
  if (auto intTy = dyn_cast<IntegerType>(ty))
    return intTy.getWidth() == 1;
  if (auto vt = dyn_cast<VectorType>(ty))
    if (auto et = dyn_cast<IntegerType>(vt.getElementType()))
      return et.getWidth() == 1;
  return false;
}

static Type vectorLaneType(VectorType vt) {
  Type elementTy = vt.getElementType();
  if (isa<VectorType>(elementTy))
    return elementTy;
  if (vt.getRank() == 1)
    return elementTy;
  return VectorType::get(vt.getShape().drop_front(), elementTy);
}

static std::optional<SmallVector<Value>> knownVectorLanes(Value value) {
  if (auto create = value.getDefiningOp<pyc::VCreateOp>())
    return SmallVector<Value>(create.getElements().begin(), create.getElements().end());
  if (auto broadcast = value.getDefiningOp<pyc::VBroadcastOp>()) {
    auto vt = dyn_cast<VectorType>(value.getType());
    if (!vt || vt.getRank() != 1)
      return std::nullopt;
    return SmallVector<Value>(static_cast<size_t>(vt.getDimSize(0)),
                              broadcast.getScalar());
  }
  return std::nullopt;
}

static bool hasFoldableVectorLane(Value lhs, Value rhs) {
  const auto lhsVT = dyn_cast<VectorType>(lhs.getType());
  const auto rhsVT = dyn_cast<VectorType>(rhs.getType());
  if (!lhsVT && !rhsVT)
    return getConstUInt(lhs).has_value() && getConstUInt(rhs).has_value();

  auto lhsLanes = lhsVT ? knownVectorLanes(lhs) : std::nullopt;
  auto rhsLanes = rhsVT ? knownVectorLanes(rhs) : std::nullopt;
  if ((lhsVT && !lhsLanes) || (rhsVT && !rhsLanes))
    return false;

  const size_t count = lhsLanes ? lhsLanes->size() : rhsLanes->size();
  for (size_t i = 0; i < count; ++i) {
    Value lhsLane = lhsLanes ? (*lhsLanes)[i] : lhs;
    Value rhsLane = rhsLanes ? (*rhsLanes)[i] : rhs;
    if (hasFoldableVectorLane(lhsLane, rhsLane))
      return true;
  }
  return false;
}

template <typename OpT>
struct FoldVectorConstantLanes : public OpRewritePattern<OpT> {
  using OpRewritePattern<OpT>::OpRewritePattern;

  LogicalResult matchAndRewrite(OpT op, PatternRewriter &rewriter) const override {
    auto resultVT = dyn_cast<VectorType>(op.getResult().getType());
    if (!resultVT || !hasFoldableVectorLane(op.getLhs(), op.getRhs()))
      return failure();

    auto lhsLanes = dyn_cast<VectorType>(op.getLhs().getType())
                        ? knownVectorLanes(op.getLhs())
                        : std::nullopt;
    auto rhsLanes = dyn_cast<VectorType>(op.getRhs().getType())
                        ? knownVectorLanes(op.getRhs())
                        : std::nullopt;
    if ((isa<VectorType>(op.getLhs().getType()) && !lhsLanes) ||
        (isa<VectorType>(op.getRhs().getType()) && !rhsLanes))
      return failure();

    const size_t count = static_cast<size_t>(resultVT.getDimSize(0));
    if ((lhsLanes && lhsLanes->size() != count) ||
        (rhsLanes && rhsLanes->size() != count))
      return failure();

    Type laneTy = vectorLaneType(resultVT);
    SmallVector<Value> lanes;
    lanes.reserve(count);
    for (size_t i = 0; i < count; ++i) {
      Value lhs = lhsLanes ? (*lhsLanes)[i] : op.getLhs();
      Value rhs = rhsLanes ? (*rhsLanes)[i] : op.getRhs();
      lanes.push_back(rewriter.create<OpT>(op.getLoc(), laneTy, lhs, rhs));
    }
    rewriter.replaceOpWithNewOp<pyc::VCreateOp>(op, op.getResult().getType(), lanes);
    return success();
  }
};

template <typename ReduceOp>
struct FoldRank2ReduceLanes : public OpRewritePattern<ReduceOp> {
  using OpRewritePattern<ReduceOp>::OpRewritePattern;

  Value combine(ReduceOp op, Type laneTy, Value lhs, Value rhs,
                PatternRewriter &rewriter) const {
    if constexpr (std::is_same_v<ReduceOp, pyc::VOrReduceOp>)
      return rewriter.create<pyc::OrOp>(op.getLoc(), laneTy, lhs, rhs);
    if constexpr (std::is_same_v<ReduceOp, pyc::VAndReduceOp>)
      return rewriter.create<pyc::AndOp>(op.getLoc(), laneTy, lhs, rhs);
    return rewriter.create<pyc::AddOp>(op.getLoc(), laneTy, lhs, rhs);
  }

  LogicalResult matchAndRewrite(ReduceOp op, PatternRewriter &rewriter) const override {
    if (!op.getDim())
      return failure();
    auto inputTy = dyn_cast<VectorType>(op.getVec().getType());
    auto resultTy = dyn_cast<VectorType>(op.getResult().getType());
    auto rows = knownVectorLanes(op.getVec());
    if (!inputTy || inputTy.getRank() != 2 || !resultTy || resultTy.getRank() != 1 ||
        !rows)
      return failure();

    const int64_t dim = *op.getDim();
    const size_t rowCount = static_cast<size_t>(inputTy.getShape()[0]);
    const size_t colCount = static_cast<size_t>(inputTy.getShape()[1]);
    if (rows->size() != rowCount || (dim != 0 && dim != 1))
      return failure();

    SmallVector<SmallVector<Value>> matrix;
    matrix.reserve(rowCount);
    for (Value row : *rows) {
      auto columns = knownVectorLanes(row);
      if (!columns || columns->size() != colCount)
        return failure();
      matrix.push_back(std::move(*columns));
    }

    const size_t outputCount = dim == 0 ? colCount : rowCount;
    if (resultTy.getDimSize(0) != static_cast<int64_t>(outputCount))
      return failure();
    bool hasConstantGroup = false;
    SmallVector<SmallVector<Value>> groups(outputCount);
    for (size_t output = 0; output < outputCount; ++output) {
      for (size_t input = 0; input < (dim == 0 ? rowCount : colCount); ++input)
        groups[output].push_back(dim == 0 ? matrix[input][output] : matrix[output][input]);
      hasConstantGroup |= llvm::all_of(groups[output],
                                       [](Value value) { return getConstUInt(value).has_value(); });
    }
    if (!hasConstantGroup)
      return failure();

    Type laneTy = vectorLaneType(resultTy);
    SmallVector<Value> lanes;
    lanes.reserve(outputCount);
    for (ArrayRef<Value> group : groups) {
      Value accumulator = group.front();
      for (Value value : group.drop_front())
        accumulator = combine(op, laneTy, accumulator, value, rewriter);
      lanes.push_back(accumulator);
    }
    rewriter.replaceOpWithNewOp<pyc::VCreateOp>(op, op.getResult().getType(), lanes);
    return success();
  }
};

struct MuxSameSelSimplify : public OpRewritePattern<pyc::MuxOp> {
  using OpRewritePattern::OpRewritePattern;

  LogicalResult matchAndRewrite(pyc::MuxOp op, PatternRewriter &rewriter) const override {
    Value sel = op.getSel();

    // sel ? (sel ? x : y) : z  ==>  sel ? x : z
    if (auto aMux = op.getA().getDefiningOp<pyc::MuxOp>()) {
      if (aMux.getSel() == sel) {
        rewriter.replaceOpWithNewOp<pyc::MuxOp>(op, op.getType(), sel, aMux.getA(), op.getB());
        return success();
      }
    }

    // sel ? x : (sel ? y : z)  ==>  sel ? x : z
    if (auto bMux = op.getB().getDefiningOp<pyc::MuxOp>()) {
      if (bMux.getSel() == sel) {
        rewriter.replaceOpWithNewOp<pyc::MuxOp>(op, op.getType(), sel, op.getA(), bMux.getB());
        return success();
      }
    }

    // (~sel) ? a : b  ==>  sel ? b : a
    if (auto n = sel.getDefiningOp<pyc::NotOp>()) {
      rewriter.replaceOpWithNewOp<pyc::MuxOp>(op, op.getType(), n.getIn(), op.getB(), op.getA());
      return success();
    }

    return failure();
  }
};

// GSIM pattern 2: compare each concatenated field with the corresponding
// slice of the constant. This is a hardware identity and belongs in MLIR so
// both C++ and Verilog consume the same transformed value graph.
struct EqConcatConstantSplit : public OpRewritePattern<pyc::EqOp> {
  using OpRewritePattern::OpRewritePattern;

  LogicalResult matchAndRewrite(pyc::EqOp op, PatternRewriter &rewriter) const override {
    if (op->hasAttr("pyc.name") || op->hasAttr("pyc.debug_keep"))
      return failure();
    auto resultType = dyn_cast<IntegerType>(op.getResult().getType());
    if (!resultType || resultType.getWidth() != 1)
      return failure();

    auto concat = op.getLhs().getDefiningOp<pyc::ConcatOp>();
    auto constant = getConstAPInt(op.getRhs());
    if (!concat || !constant) {
      concat = op.getRhs().getDefiningOp<pyc::ConcatOp>();
      constant = getConstAPInt(op.getLhs());
    }
    if (!concat || !constant || concat.getInputs().size() < 2 ||
        concat->hasAttr("pyc.name") || concat->hasAttr("pyc.debug_keep"))
      return failure();
    auto concatType = dyn_cast<IntegerType>(concat.getResult().getType());
    if (!concatType || constant->getBitWidth() != concatType.getWidth())
      return failure();

    unsigned offset = concatType.getWidth();
    Value allEqual;
    for (Value input : concat.getInputs()) {
      auto inputType = dyn_cast<IntegerType>(input.getType());
      if (!inputType || inputType.getWidth() > offset)
        return failure();
      offset -= inputType.getWidth();
      llvm::APInt field = constant->extractBits(inputType.getWidth(), offset);
      Value fieldConstant = rewriter.create<pyc::ConstantOp>(
          op.getLoc(), inputType, IntegerAttr::get(inputType, field));
      Value fieldEqual = rewriter.create<pyc::EqOp>(
          op.getLoc(), resultType, input, fieldConstant);
      allEqual = allEqual
                     ? rewriter.create<pyc::AndOp>(op.getLoc(), resultType,
                                                    allEqual, fieldEqual).getResult()
                     : fieldEqual;
    }
    if (offset != 0)
      return failure();
    rewriter.replaceOp(op, allEqual);
    return success();
  }
};

// A disjoint shifted-OR is a concatenation with possible zero-filled gaps.
// Split its equality only when both fields are zero-extended narrow values;
// otherwise the OR may have overlapping set bits or truncated source bits.
struct EqShiftedOrConstantSplit : public OpRewritePattern<pyc::EqOp> {
  using OpRewritePattern::OpRewritePattern;

  LogicalResult matchAndRewrite(pyc::EqOp op, PatternRewriter &rewriter) const override {
    if (op->hasAttr("pyc.name") || op->hasAttr("pyc.debug_keep") ||
        op.getResult().getType() != rewriter.getI1Type())
      return failure();
    auto merged = stripAlias(op.getLhs()).getDefiningOp<pyc::OrOp>();
    auto constant = getConstAPInt(stripAlias(op.getRhs()));
    if (!merged || !constant) {
      merged = stripAlias(op.getRhs()).getDefiningOp<pyc::OrOp>();
      constant = getConstAPInt(stripAlias(op.getLhs()));
    }
    if (!merged || !constant)
      return failure();
    auto resultType = dyn_cast<IntegerType>(merged.getResult().getType());
    if (!resultType || constant->getBitWidth() != resultType.getWidth())
      return failure();
    for (auto [shiftedValue, lowValue] :
         {std::pair<Value, Value>{merged.getLhs(), merged.getRhs()},
          std::pair<Value, Value>{merged.getRhs(), merged.getLhs()}}) {
      auto shift = stripAlias(shiftedValue).getDefiningOp<pyc::ShliOp>();
      auto high = shift ? stripAlias(shift.getIn()).getDefiningOp<pyc::ZextOp>()
                        : pyc::ZextOp{};
      auto low = stripAlias(lowValue).getDefiningOp<pyc::ZextOp>();
      if (!shift || !high || !low)
        continue;
      auto highType = dyn_cast<IntegerType>(high.getIn().getType());
      auto lowType = dyn_cast<IntegerType>(low.getIn().getType());
      if (!highType || !lowType)
        continue;
      unsigned width = resultType.getWidth();
      uint64_t amount = shift.getAmount();
      unsigned highWidth = highType.getWidth(), lowWidth = lowType.getWidth();
      if (amount < lowWidth || amount >= width || highWidth > width - amount)
        continue;
      llvm::APInt allowed = llvm::APInt::getLowBitsSet(width, lowWidth) |
                            llvm::APInt::getLowBitsSet(width, highWidth)
                                .shl(static_cast<unsigned>(amount));
      if (!(*constant & ~allowed).isZero()) {
        rewriter.replaceOp(op, constInt(rewriter, op.getLoc(),
                                        rewriter.getI1Type(), llvm::APInt(1, 0)));
        return success();
      }
      Value highConstant = constInt(
          rewriter, op.getLoc(), highType,
          constant->lshr(static_cast<unsigned>(amount)).trunc(highWidth));
      Value lowConstant = constInt(rewriter, op.getLoc(), lowType,
                                   constant->trunc(lowWidth));
      Value highEqual = rewriter.create<pyc::EqOp>(
          op.getLoc(), rewriter.getI1Type(), high.getIn(), highConstant);
      Value lowEqual = rewriter.create<pyc::EqOp>(
          op.getLoc(), rewriter.getI1Type(), low.getIn(), lowConstant);
      rewriter.replaceOpWithNewOp<pyc::AndOp>(
          op, rewriter.getI1Type(), highEqual, lowEqual);
      return success();
    }
    return failure();
  }
};

// GSIM pattern 1: bit k of (1 << amount) is exactly (amount == k).
struct ExtractOneHotShift : public OpRewritePattern<pyc::ExtractOp> {
  using OpRewritePattern::OpRewritePattern;

  LogicalResult matchAndRewrite(pyc::ExtractOp op, PatternRewriter &rewriter) const override {
    if (op->hasAttr("pyc.name") || op->hasAttr("pyc.debug_keep") ||
        op.getResult().getType() != rewriter.getI1Type())
      return failure();
    uint64_t bit = op.getLsb();
    if (op.getMsb().value_or(bit) != bit)
      return failure();
    auto shift = stripAlias(op.getIn()).getDefiningOp<pyc::ShlOp>();
    if (!shift || shift->hasAttr("pyc.name") || shift->hasAttr("pyc.debug_keep"))
      return failure();
    auto one = getConstAPInt(stripAlias(shift.getIn()));
    if (!one || !one->isOne())
      return failure();
    auto amountType = dyn_cast<IntegerType>(shift.getAmount().getType());
    if (!amountType)
      return failure();
    unsigned amountWidth = amountType.getWidth();
    if (amountWidth < 64 && bit >= (uint64_t{1} << amountWidth)) {
      Value zero = constInt(rewriter, op.getLoc(), op.getResult().getType(),
                            llvm::APInt(1, 0));
      rewriter.replaceOp(op, zero);
      return success();
    }
    Value bitConstant = rewriter.create<pyc::ConstantOp>(
        op.getLoc(), amountType,
        IntegerAttr::get(amountType, llvm::APInt(amountWidth, bit)));
    rewriter.replaceOpWithNewOp<pyc::EqOp>(op, rewriter.getI1Type(),
                                           shift.getAmount(), bitConstant);
    return success();
  }
};

// Push a demanded bit range through a concatenation when it lies wholly in
// one field. This creates the hardware-side slice used by bit-level simulation
// activity analysis without changing the value observed by either backend.
struct ExtractThroughConcat : public OpRewritePattern<pyc::ExtractOp> {
  using OpRewritePattern::OpRewritePattern;

  LogicalResult matchAndRewrite(pyc::ExtractOp op, PatternRewriter &rewriter) const override {
    if (op->hasAttr("pyc.name") || op->hasAttr("pyc.debug_keep"))
      return failure();
    auto concat = op.getIn().getDefiningOp<pyc::ConcatOp>();
    auto resultType = dyn_cast<IntegerType>(op.getResult().getType());
    if (!concat || !resultType || concat->hasAttr("pyc.name") ||
        concat->hasAttr("pyc.debug_keep"))
      return failure();
    uint64_t first = op.getLsb();
    uint64_t last = first + resultType.getWidth();
    uint64_t offset = 0;
    SmallVector<Value> lowParts;
    uint64_t selectedWidth = 0;
    for (Value input : llvm::reverse(concat.getInputs())) {
      auto inputType = dyn_cast<IntegerType>(input.getType());
      if (!inputType)
        return failure();
      uint64_t end = offset + inputType.getWidth();
      uint64_t selectedFirst = std::max(first, offset);
      uint64_t selectedLast = std::min(last, end);
      if (selectedFirst < selectedLast) {
        uint64_t localFirst = selectedFirst - offset;
        uint64_t width = selectedLast - selectedFirst;
        Value part = input;
        if (localFirst != 0 || width != inputType.getWidth()) {
          auto partType = IntegerType::get(op.getContext(), width);
          part = rewriter.create<pyc::ExtractOp>(
              op.getLoc(), partType, input, localFirst, IntegerAttr{});
        }
        lowParts.push_back(part);
        selectedWidth += width;
      }
      offset = end;
    }
    if (selectedWidth != resultType.getWidth() || lowParts.empty())
      return failure();
    if (lowParts.size() == 1) {
      rewriter.replaceOp(op, lowParts.front());
      return success();
    }
    std::reverse(lowParts.begin(), lowParts.end());
    rewriter.replaceOpWithNewOp<pyc::ConcatOp>(op, resultType, lowParts);
    return success();
  }
};

struct ExtractThroughNot : public OpRewritePattern<pyc::ExtractOp> {
  using OpRewritePattern::OpRewritePattern;

  LogicalResult matchAndRewrite(pyc::ExtractOp op, PatternRewriter &rewriter) const override {
    if (op->hasAttr("pyc.name") || op->hasAttr("pyc.debug_keep"))
      return failure();
    auto invert = op.getIn().getDefiningOp<pyc::NotOp>();
    if (!invert || !invert.getResult().hasOneUse() ||
        invert->hasAttr("pyc.name") || invert->hasAttr("pyc.debug_keep"))
      return failure();
    Value part = rewriter.create<pyc::ExtractOp>(
        op.getLoc(), op.getResult().getType(), invert.getIn(), op.getLsb(),
        op.getMsbAttr());
    rewriter.replaceOpWithNewOp<pyc::NotOp>(op, part);
    return success();
  }
};

// Partial slice readers of a width cast need only source bits or extension
// fill. Rewrite all readers together so the original cast becomes dead.
struct ExtractThroughWidthCast : public OpRewritePattern<pyc::ExtractOp> {
  using OpRewritePattern<pyc::ExtractOp>::OpRewritePattern;

  LogicalResult matchAndRewrite(pyc::ExtractOp get,
                                PatternRewriter &rewriter) const override {
    Operation *castOperation = get.getIn().getDefiningOp();
    if (!castOperation ||
        castOperation->hasAttr("pyc.name") ||
        castOperation->hasAttr("pyc.debug_keep"))
      return failure();
    bool truncate = isa<pyc::TruncOp>(castOperation);
    bool zeroExtend = isa<pyc::ZextOp>(castOperation);
    bool signExtend = isa<pyc::SextOp>(castOperation);
    if (!truncate && !zeroExtend && !signExtend)
      return failure();
    Value input = castOperation->getOperand(0);
    auto inputType = dyn_cast<IntegerType>(input.getType());
    auto castType = dyn_cast<IntegerType>(get.getIn().getType());
    if (!inputType || !castType)
      return failure();
    uint64_t inputWidth = inputType.getWidth();
    SmallVector<pyc::ExtractOp> readers;
    for (OpOperand &use : get.getIn().getUses()) {
      auto reader = dyn_cast<pyc::ExtractOp>(use.getOwner());
      if (!reader || reader.getIn() != get.getIn() ||
          reader->hasAttr("pyc.name") ||
          reader->hasAttr("pyc.debug_keep"))
        return failure();
      auto resultType = dyn_cast<IntegerType>(reader.getResult().getType());
      if (!resultType ||
          (reader.getLsb() == 0 &&
           resultType.getWidth() == castType.getWidth()))
        return failure();
      readers.push_back(reader);
    }
    if (readers.empty())
      return failure();
    rewriter.setInsertionPoint(castOperation);
    for (pyc::ExtractOp reader : readers) {
      uint64_t begin = reader.getLsb();
      uint64_t width =
          cast<IntegerType>(reader.getResult().getType()).getWidth();
      uint64_t end = begin + width;
      auto slice = [&](uint64_t lsb, uint64_t bits) -> Value {
        if (lsb == 0 && bits == inputWidth)
          return input;
        Type type = IntegerType::get(get.getContext(), bits);
        return rewriter.create<pyc::ExtractOp>(
            reader.getLoc(), type, input, lsb, IntegerAttr{});
      };
      Value replacement;
      if (truncate || end <= inputWidth) {
        replacement = slice(begin, width);
      } else if (zeroExtend) {
        if (begin >= inputWidth) {
          replacement = constInt(
              rewriter, reader.getLoc(), reader.getResult().getType(),
              llvm::APInt(static_cast<unsigned>(width), 0));
        } else {
          Value data = slice(begin, inputWidth - begin);
          Type fillType = IntegerType::get(get.getContext(), end - inputWidth);
          Value fill = constInt(
              rewriter, reader.getLoc(), fillType,
              llvm::APInt(static_cast<unsigned>(end - inputWidth), 0));
          replacement = rewriter.create<pyc::ConcatOp>(
              reader.getLoc(), reader.getResult().getType(),
              SmallVector<Value>{fill, data});
        }
      } else {
        Value data = begin >= inputWidth
                         ? slice(inputWidth - 1, 1)
                         : slice(begin, inputWidth - begin);
        replacement = data.getType() == reader.getResult().getType()
                          ? data
                          : rewriter.create<pyc::SextOp>(
                                reader.getLoc(), reader.getResult().getType(),
                                data).getResult();
      }
      rewriter.replaceOp(reader, replacement);
    }
    rewriter.eraseOp(castOperation);
    return success();
  }
};

static bool allUsesAreDisjointSlices(Value value) {
  auto inputTy = dyn_cast<IntegerType>(value.getType());
  if (!inputTy)
    return false;
  SmallVector<std::pair<uint64_t, uint64_t>> ranges;
  for (OpOperand &use : value.getUses()) {
    auto slice = dyn_cast<pyc::ExtractOp>(use.getOwner());
    if (!slice || slice.getIn() != value ||
        slice->hasAttr("pyc.name") || slice->hasAttr("pyc.debug_keep"))
      return false;
    auto outTy = dyn_cast<IntegerType>(slice.getResult().getType());
    if (!outTy)
      return false;
    uint64_t first = slice.getLsb();
    ranges.emplace_back(first, first + outTy.getWidth());
  }
  if (ranges.size() < 2)
    return false;
  std::sort(ranges.begin(), ranges.end());
  for (size_t i = 1; i < ranges.size(); ++i)
    if (ranges[i - 1].second > ranges[i].first)
      return false;
  return ranges.back().second <= inputTy.getWidth();
}

// GSIM's node segmentation applies to state as well as pure expressions.
// When an unnamed scalar register is observed only through slices, split it
// at every slice boundary and reuse segments for overlapping observations.
// Each segment retains the original clock, reset and enable.
// This rewrite lives in the hardware pipeline so Verilog and C++ see the same
// state and the usual clock-domain/cycle verifiers run afterward.
struct SplitRegisterBySlices : public OpRewritePattern<pyc::RegOp> {
  using OpRewritePattern<pyc::RegOp>::OpRewritePattern;

  LogicalResult matchAndRewrite(pyc::RegOp op,
                                PatternRewriter &rewriter) const override {
    if (!op->getAttrs().empty())
      return failure();
    auto originalTy = dyn_cast<IntegerType>(op.getQ().getType());
    if (!originalTy)
      return failure();
    SmallVector<pyc::ExtractOp> slices;
    SmallVector<uint64_t> cuts;
    for (OpOperand &use : op.getQ().getUses()) {
      auto slice = dyn_cast<pyc::ExtractOp>(use.getOwner());
      if (!slice || slice.getIn() != op.getQ() ||
          slice->hasAttr("pyc.name") || slice->hasAttr("pyc.debug_keep"))
        return failure();
      auto resultTy = dyn_cast<IntegerType>(slice.getResult().getType());
      if (!resultTy)
        return failure();
      uint64_t begin = slice.getLsb();
      uint64_t end = begin + resultTy.getWidth();
      if (end > originalTy.getWidth())
        return failure();
      cuts.push_back(begin);
      cuts.push_back(end);
      slices.push_back(slice);
    }
    if (slices.size() < 2)
      return failure();
    llvm::sort(cuts);
    cuts.erase(std::unique(cuts.begin(), cuts.end()), cuts.end());
    if (cuts.size() < 3)
      return failure();
    std::sort(slices.begin(), slices.end(), [](pyc::ExtractOp a,
                                               pyc::ExtractOp b) {
      return a.getLsb() < b.getLsb();
    });
    std::map<uint64_t, int> coverageDeltas;
    for (pyc::ExtractOp slice : slices) {
      uint64_t begin = slice.getLsb();
      uint64_t end = begin +
          cast<IntegerType>(slice.getResult().getType()).getWidth();
      ++coverageDeltas[begin];
      --coverageDeltas[end];
    }
    rewriter.setInsertionPoint(op);
    struct Segment {
      uint64_t begin;
      uint64_t end;
      Value q;
    };
    SmallVector<Segment> segments;
    int coverage = 0;
    for (size_t i = 1; i < cuts.size(); ++i) {
      uint64_t begin = cuts[i - 1], end = cuts[i];
      coverage += coverageDeltas[begin];
      if (coverage == 0)
        continue;
      Type type = IntegerType::get(op.getContext(), end - begin);
      Value nextPart = rewriter.create<pyc::ExtractOp>(
          op.getLoc(), type, op.getNext(), begin, IntegerAttr{});
      Value initPart = rewriter.create<pyc::ExtractOp>(
          op.getLoc(), type, op.getInit(), begin, IntegerAttr{});
      Value q = rewriter.create<pyc::RegOp>(
          op.getLoc(), type, op.getClk(), op.getRst(), op.getEn(),
          nextPart, initPart).getQ();
      segments.push_back({begin, end, q});
    }
    for (pyc::ExtractOp slice : slices) {
      uint64_t begin = slice.getLsb();
      uint64_t end = begin +
          cast<IntegerType>(slice.getResult().getType()).getWidth();
      SmallVector<Value> highToLow;
      auto first = std::lower_bound(
          segments.begin(), segments.end(), begin,
          [](const Segment &segment, uint64_t bit) {
            return segment.begin < bit;
          });
      auto last = std::lower_bound(
          first, segments.end(), end,
          [](const Segment &segment, uint64_t bit) {
            return segment.begin < bit;
          });
      for (auto it = last; it != first;) {
        --it;
        highToLow.push_back(it->q);
      }
      assert(!highToLow.empty() && "every reader must cover a segment");
      if (highToLow.size() == 1)
        rewriter.replaceOp(slice, highToLow.front());
      else {
        rewriter.setInsertionPoint(slice);
        rewriter.replaceOpWithNewOp<pyc::ConcatOp>(
            slice, slice.getResult().getType(), highToLow);
      }
    }
    rewriter.eraseOp(op);
    return success();
  }
};

// A vector register with only constant-index outer-lane readers can keep just
// the observed sub-vectors. Repeating this rewrite on nested readers reaches
// scalar leaves without changing the clock/reset semantics.
struct SplitVectorRegisterLanes : public OpRewritePattern<pyc::RegOp> {
  using OpRewritePattern<pyc::RegOp>::OpRewritePattern;

  LogicalResult matchAndRewrite(pyc::RegOp op,
                                PatternRewriter &rewriter) const override {
    if (!op->getAttrs().empty())
      return failure();
    auto vectorTy = dyn_cast<VectorType>(op.getQ().getType());
    if (!vectorTy || vectorTy.getRank() < 1 ||
        !isa<IntegerType>(vectorTy.getElementType()))
      return failure();
    std::map<uint64_t, SmallVector<pyc::VGetOp>> readers;
    for (OpOperand &use : op.getQ().getUses()) {
      auto get = dyn_cast<pyc::VGetOp>(use.getOwner());
      if (!get || get.getVec() != op.getQ() ||
          get->hasAttr("pyc.name") || get->hasAttr("pyc.debug_keep"))
        return failure();
      uint64_t index = get.getIndex();
      if (index >= static_cast<uint64_t>(vectorTy.getDimSize(0)))
        return failure();
      readers[index].push_back(get);
    }
    if (readers.empty())
      return failure();
    rewriter.setInsertionPoint(op);
    for (auto &[index, uses] : readers) {
      IntegerAttr indexAttr = rewriter.getI64IntegerAttr(index);
      Type type = uses.front().getResult().getType();
      Value next = rewriter.create<pyc::VGetOp>(
          op.getLoc(), type, op.getNext(), indexAttr);
      Value init = rewriter.create<pyc::VGetOp>(
          op.getLoc(), type, op.getInit(), indexAttr);
      Value q = rewriter.create<pyc::RegOp>(
          op.getLoc(), type, op.getClk(), op.getRst(), op.getEn(),
          next, init).getQ();
      for (pyc::VGetOp get : uses)
        rewriter.replaceOp(get, q);
    }
    rewriter.eraseOp(op);
    return success();
  }
};

// Materialize only an observed outer lane of an elementwise vector value.
// All users must be lane readers so the original aggregate becomes dead.
// This also exposes lane readers on vector state to the selective state split.
template <typename ElementwiseOp>
struct VGetThroughElementwise : public OpRewritePattern<pyc::VGetOp> {
  using OpRewritePattern<pyc::VGetOp>::OpRewritePattern;

  LogicalResult matchAndRewrite(pyc::VGetOp get,
                                PatternRewriter &rewriter) const override {
    if (get->hasAttr("pyc.name") || get->hasAttr("pyc.debug_keep"))
      return failure();
    auto elementwise = get.getVec().getDefiningOp<ElementwiseOp>();
    auto vectorTy = dyn_cast<VectorType>(get.getVec().getType());
    if (!elementwise || !vectorTy || vectorTy.getRank() < 1 ||
        elementwise->hasAttr("pyc.name") ||
        elementwise->hasAttr("pyc.debug_keep"))
      return failure();
    std::map<uint64_t, SmallVector<pyc::VGetOp>> readers;
    for (OpOperand &use : elementwise.getResult().getUses()) {
      auto reader = dyn_cast<pyc::VGetOp>(use.getOwner());
      if (!reader || reader.getVec() != elementwise.getResult() ||
          reader->hasAttr("pyc.name") ||
          reader->hasAttr("pyc.debug_keep") ||
          reader.getIndex() >=
              static_cast<uint64_t>(vectorTy.getDimSize(0)))
        return failure();
      readers[reader.getIndex()].push_back(reader);
    }
    if (readers.empty())
      return failure();
    for (Value operand : elementwise->getOperands())
      if (auto operandTy = dyn_cast<VectorType>(operand.getType()))
        if (operandTy.getShape() != vectorTy.getShape())
          return failure();
    rewriter.setInsertionPoint(elementwise);
    for (auto &[index, uses] : readers) {
      SmallVector<Value> operands;
      for (Value operand : elementwise->getOperands()) {
        if (auto operandTy = dyn_cast<VectorType>(operand.getType())) {
          auto innerShape = operandTy.getShape().drop_front();
          Type innerType = innerShape.empty()
                               ? operandTy.getElementType()
                               : Type(VectorType::get(innerShape,
                                                      operandTy.getElementType()));
          operands.push_back(rewriter.create<pyc::VGetOp>(
              uses.front().getLoc(), innerType, operand,
              uses.front().getIndexAttr()));
        } else {
          operands.push_back(operand);
        }
      }
      OperationState state(uses.front().getLoc(), elementwise->getName());
      state.addOperands(operands);
      state.addTypes(uses.front().getResult().getType());
      for (NamedAttribute attr : elementwise->getAttrs())
        state.addAttribute(attr.getName(), attr.getValue());
      Value lane = rewriter.create(state)->getResult(0);
      for (pyc::VGetOp reader : uses)
        rewriter.replaceOp(reader, lane);
    }
    rewriter.eraseOp(elementwise);
    return success();
  }
};

template <typename BitwiseOp>
struct SplitBitwiseBySlices : public OpRewritePattern<BitwiseOp> {
  explicit SplitBitwiseBySlices(MLIRContext *context)
      : OpRewritePattern<BitwiseOp>(context, /*benefit=*/2) {}

  LogicalResult matchAndRewrite(BitwiseOp op,
                                PatternRewriter &rewriter) const override {
    if (op->hasAttr("pyc.name") || op->hasAttr("pyc.debug_keep"))
      return failure();
    auto type = dyn_cast<IntegerType>(op.getResult().getType());
    if (!type)
      return failure();
    for (Value operand : op->getOperands())
      if (operand.getType() != type)
        return failure();
    SmallVector<pyc::ExtractOp> readers;
    SmallVector<uint64_t> cuts;
    for (OpOperand &use : op.getResult().getUses()) {
      auto slice = dyn_cast<pyc::ExtractOp>(use.getOwner());
      if (!slice || slice.getIn() != op.getResult() ||
          slice->hasAttr("pyc.name") || slice->hasAttr("pyc.debug_keep"))
        return failure();
      auto sliceType = dyn_cast<IntegerType>(slice.getResult().getType());
      if (!sliceType)
        return failure();
      uint64_t begin = slice.getLsb();
      uint64_t end = begin + sliceType.getWidth();
      if (end > type.getWidth())
        return failure();
      cuts.push_back(begin);
      cuts.push_back(end);
      readers.push_back(slice);
    }
    if (readers.size() < 2)
      return failure();
    llvm::sort(cuts);
    cuts.erase(std::unique(cuts.begin(), cuts.end()), cuts.end());
    if (cuts.size() < 3)
      return failure();
    std::map<uint64_t, int> coverageDeltas;
    for (pyc::ExtractOp slice : readers) {
      uint64_t begin = slice.getLsb();
      uint64_t end = begin +
          cast<IntegerType>(slice.getResult().getType()).getWidth();
      ++coverageDeltas[begin];
      --coverageDeltas[end];
    }

    struct Segment {
      uint64_t begin;
      uint64_t end;
      Value result;
    };
    SmallVector<Segment> segments;
    rewriter.setInsertionPoint(op);
    int coverage = 0;
    for (size_t i = 1; i < cuts.size(); ++i) {
      uint64_t begin = cuts[i - 1], end = cuts[i];
      coverage += coverageDeltas[begin];
      if (coverage == 0)
        continue;
      Type segmentType = IntegerType::get(op.getContext(), end - begin);
      SmallVector<Value> operands;
      for (Value operand : op->getOperands()) {
        operands.push_back(rewriter.create<pyc::ExtractOp>(
            op.getLoc(), segmentType, operand, begin, IntegerAttr{}));
      }
      OperationState state(op.getLoc(), op->getName());
      state.addOperands(operands);
      state.addTypes(segmentType);
      for (NamedAttribute attr : op->getAttrs())
        state.addAttribute(attr.getName(), attr.getValue());
      segments.push_back({begin, end, rewriter.create(state)->getResult(0)});
    }
    for (pyc::ExtractOp slice : readers) {
      uint64_t begin = slice.getLsb();
      uint64_t end = begin +
          cast<IntegerType>(slice.getResult().getType()).getWidth();
      SmallVector<Value> highToLow;
      auto first = std::lower_bound(
          segments.begin(), segments.end(), begin,
          [](const Segment &segment, uint64_t bit) {
            return segment.begin < bit;
          });
      auto last = std::lower_bound(
          first, segments.end(), end,
          [](const Segment &segment, uint64_t bit) {
            return segment.begin < bit;
          });
      for (auto it = last; it != first;) {
        --it;
        highToLow.push_back(it->result);
      }
      assert(!highToLow.empty() && "every reader must cover at least one segment");
      if (highToLow.size() == 1)
        rewriter.replaceOp(slice, highToLow.front());
      else {
        rewriter.setInsertionPoint(slice);
        rewriter.replaceOpWithNewOp<pyc::ConcatOp>(
            slice, slice.getResult().getType(), highToLow);
      }
    }
    rewriter.eraseOp(op);
    return success();
  }
};

template <typename BitwiseOp>
struct ExtractThroughBitwise : public OpRewritePattern<pyc::ExtractOp> {
  using OpRewritePattern<pyc::ExtractOp>::OpRewritePattern;

  LogicalResult matchAndRewrite(pyc::ExtractOp op, PatternRewriter &rewriter) const override {
    if (op->hasAttr("pyc.name") || op->hasAttr("pyc.debug_keep"))
      return failure();
    auto bitwise = op.getIn().getDefiningOp<BitwiseOp>();
    if (!bitwise ||
        (!bitwise.getResult().hasOneUse() &&
         !allUsesAreDisjointSlices(bitwise.getResult())) ||
        bitwise->hasAttr("pyc.name") || bitwise->hasAttr("pyc.debug_keep") ||
        !isa<IntegerType>(op.getResult().getType()))
      return failure();
    Value lhs = rewriter.create<pyc::ExtractOp>(
        op.getLoc(), op.getResult().getType(), bitwise.getLhs(),
        op.getLsb(), op.getMsbAttr());
    Value rhs = rewriter.create<pyc::ExtractOp>(
        op.getLoc(), op.getResult().getType(), bitwise.getRhs(),
        op.getLsb(), op.getMsbAttr());
    rewriter.replaceOpWithNewOp<BitwiseOp>(
        op, op.getResult().getType(), lhs, rhs);
    return success();
  }
};

// A scalar mux read through separated bit slices needs only the selected
// segments. Split at every reader boundary so overlapping readers share the
// same mux segment. A contiguous observed prefix stays with the cheaper
// single narrowed mux rewrite below.
struct SplitMuxBySlices : public OpRewritePattern<pyc::MuxOp> {
  explicit SplitMuxBySlices(MLIRContext *context)
      : OpRewritePattern<pyc::MuxOp>(context, /*benefit=*/3) {}

  LogicalResult matchAndRewrite(pyc::MuxOp op,
                                PatternRewriter &rewriter) const override {
    if (op->hasAttr("pyc.name") || op->hasAttr("pyc.debug_keep"))
      return failure();
    auto type = dyn_cast<IntegerType>(op.getResult().getType());
    auto selectorType = dyn_cast<IntegerType>(op.getSel().getType());
    if (!type || !selectorType || selectorType.getWidth() != 1 ||
        op.getA().getType() != type || op.getB().getType() != type)
      return failure();
    SmallVector<pyc::ExtractOp> readers;
    SmallVector<uint64_t> cuts;
    std::map<uint64_t, int> coverageDeltas;
    for (OpOperand &use : op.getResult().getUses()) {
      auto reader = dyn_cast<pyc::ExtractOp>(use.getOwner());
      if (!reader || reader.getIn() != op.getResult() ||
          reader->hasAttr("pyc.name") ||
          reader->hasAttr("pyc.debug_keep"))
        return failure();
      auto resultType = dyn_cast<IntegerType>(reader.getResult().getType());
      if (!resultType)
        return failure();
      uint64_t begin = reader.getLsb();
      uint64_t end = begin + resultType.getWidth();
      if (end > type.getWidth())
        return failure();
      cuts.push_back(begin);
      cuts.push_back(end);
      ++coverageDeltas[begin];
      --coverageDeltas[end];
      readers.push_back(reader);
    }
    if (readers.size() < 2)
      return failure();
    llvm::sort(cuts);
    cuts.erase(std::unique(cuts.begin(), cuts.end()), cuts.end());
    if (cuts.size() < 4)
      return failure();
    bool hasGap = false;
    int coverage = 0;
    for (size_t i = 1; i < cuts.size(); ++i) {
      coverage += coverageDeltas[cuts[i - 1]];
      hasGap |= coverage == 0 && i + 1 < cuts.size();
    }
    if (!hasGap)
      return failure();

    struct Segment {
      uint64_t begin;
      Value result;
    };
    SmallVector<Segment> segments;
    rewriter.setInsertionPoint(op);
    coverage = 0;
    for (size_t i = 1; i < cuts.size(); ++i) {
      uint64_t begin = cuts[i - 1], end = cuts[i];
      coverage += coverageDeltas[begin];
      if (!coverage)
        continue;
      Type segmentType = IntegerType::get(op.getContext(), end - begin);
      Value a = rewriter.create<pyc::ExtractOp>(
          op.getLoc(), segmentType, op.getA(), begin, IntegerAttr{});
      Value b = rewriter.create<pyc::ExtractOp>(
          op.getLoc(), segmentType, op.getB(), begin, IntegerAttr{});
      Value selected = rewriter.create<pyc::MuxOp>(
          op.getLoc(), segmentType, op.getSel(), a, b);
      segments.push_back({begin, selected});
    }
    for (pyc::ExtractOp reader : readers) {
      uint64_t begin = reader.getLsb();
      uint64_t end = begin +
          cast<IntegerType>(reader.getResult().getType()).getWidth();
      SmallVector<Value> highToLow;
      auto first = std::lower_bound(
          segments.begin(), segments.end(), begin,
          [](const Segment &segment, uint64_t bit) {
            return segment.begin < bit;
          });
      auto last = std::lower_bound(
          first, segments.end(), end,
          [](const Segment &segment, uint64_t bit) {
            return segment.begin < bit;
          });
      for (auto it = last; it != first;) {
        --it;
        highToLow.push_back(it->result);
      }
      assert(!highToLow.empty() && "every mux reader must cover a segment");
      if (highToLow.size() == 1)
        rewriter.replaceOp(reader, highToLow.front());
      else {
        rewriter.setInsertionPoint(reader);
        rewriter.replaceOpWithNewOp<pyc::ConcatOp>(
            reader, reader.getResult().getType(), highToLow);
      }
    }
    rewriter.eraseOp(op);
    return success();
  }
};

struct ExtractThroughMux : public OpRewritePattern<pyc::ExtractOp> {
  using OpRewritePattern::OpRewritePattern;

  LogicalResult matchAndRewrite(pyc::ExtractOp op, PatternRewriter &rewriter) const override {
    if (op->hasAttr("pyc.name") || op->hasAttr("pyc.debug_keep"))
      return failure();
    auto mux = op.getIn().getDefiningOp<pyc::MuxOp>();
    if (!mux || !mux.getResult().hasOneUse() ||
        mux->hasAttr("pyc.name") || mux->hasAttr("pyc.debug_keep") ||
        !isa<IntegerType>(op.getResult().getType()))
      return failure();
    Value a = rewriter.create<pyc::ExtractOp>(
        op.getLoc(), op.getResult().getType(), mux.getA(),
        op.getLsb(), op.getMsbAttr());
    Value b = rewriter.create<pyc::ExtractOp>(
        op.getLoc(), op.getResult().getType(), mux.getB(),
        op.getLsb(), op.getMsbAttr());
    rewriter.replaceOpWithNewOp<pyc::MuxOp>(
        op, op.getResult().getType(), mux.getSel(), a, b);
    return success();
  }
};

// A scalar mux with several slice readers needs only the prefix through the
// highest observed bit. Narrow both arms once and share the selected prefix.
struct NarrowMultiReaderMux : public OpRewritePattern<pyc::MuxOp> {
  explicit NarrowMultiReaderMux(MLIRContext *context)
      : OpRewritePattern<pyc::MuxOp>(context, /*benefit=*/2) {}

  LogicalResult matchAndRewrite(pyc::MuxOp op,
                                PatternRewriter &rewriter) const override {
    if (op->hasAttr("pyc.name") || op->hasAttr("pyc.debug_keep") ||
        op.getResult().hasOneUse())
      return failure();
    auto originalTy = dyn_cast<IntegerType>(op.getResult().getType());
    auto selectorTy = dyn_cast<IntegerType>(op.getSel().getType());
    if (!originalTy || !selectorTy || selectorTy.getWidth() != 1 ||
        op.getA().getType() != originalTy ||
        op.getB().getType() != originalTy)
      return failure();
    SmallVector<pyc::ExtractOp> extracts;
    SmallVector<pyc::TruncOp> truncs;
    uint64_t neededWidth = 0;
    for (OpOperand &use : op.getResult().getUses()) {
      if (auto extract = dyn_cast<pyc::ExtractOp>(use.getOwner())) {
        auto resultTy = dyn_cast<IntegerType>(extract.getResult().getType());
        if (!resultTy || extract.getIn() != op.getResult() ||
            extract->hasAttr("pyc.name") ||
            extract->hasAttr("pyc.debug_keep"))
          return failure();
        neededWidth = std::max<uint64_t>(
            neededWidth, static_cast<uint64_t>(extract.getLsb()) +
                             resultTy.getWidth());
        extracts.push_back(extract);
      } else if (auto trunc = dyn_cast<pyc::TruncOp>(use.getOwner())) {
        auto resultTy = dyn_cast<IntegerType>(trunc.getResult().getType());
        if (!resultTy || trunc.getIn() != op.getResult() ||
            trunc->hasAttr("pyc.name") ||
            trunc->hasAttr("pyc.debug_keep"))
          return failure();
        neededWidth = std::max<uint64_t>(neededWidth, resultTy.getWidth());
        truncs.push_back(trunc);
      } else {
        return failure();
      }
    }
    if (!neededWidth || neededWidth >= originalTy.getWidth())
      return failure();
    Type narrowTy = IntegerType::get(op.getContext(), neededWidth);
    rewriter.setInsertionPoint(op);
    Value a = rewriter.create<pyc::TruncOp>(op.getLoc(), narrowTy,
                                            op.getA());
    Value b = rewriter.create<pyc::TruncOp>(op.getLoc(), narrowTy,
                                            op.getB());
    Value narrowed = rewriter.create<pyc::MuxOp>(
        op.getLoc(), narrowTy, op.getSel(), a, b);
    for (pyc::ExtractOp extract : extracts) {
      Type resultTy = extract.getResult().getType();
      Value result = resultTy == narrowTy && extract.getLsb() == 0
                         ? narrowed
                         : rewriter.create<pyc::ExtractOp>(
                               extract.getLoc(), resultTy, narrowed,
                               extract.getLsb(), extract.getMsbAttr())
                               .getResult();
      rewriter.replaceOp(extract, result);
    }
    for (pyc::TruncOp trunc : truncs) {
      Type resultTy = trunc.getResult().getType();
      Value result = resultTy == narrowTy
                         ? narrowed
                         : rewriter.create<pyc::TruncOp>(
                               trunc.getLoc(), resultTy, narrowed)
                               .getResult();
      rewriter.replaceOp(trunc, result);
    }
    rewriter.eraseOp(op);
    return success();
  }
};

// Split a fixed shift at the observed slice boundary. Data bits become an
// input extract; low/high fill becomes a zero constant or sign extension.
// This changes the hardware value graph and therefore feeds both backends.
template <typename ShiftOp>
struct ExtractThroughImmediateShift : public OpRewritePattern<pyc::ExtractOp> {
  using OpRewritePattern<pyc::ExtractOp>::OpRewritePattern;

  LogicalResult matchAndRewrite(pyc::ExtractOp op,
                                PatternRewriter &rewriter) const override {
    if (op->hasAttr("pyc.name") || op->hasAttr("pyc.debug_keep"))
      return failure();
    auto shift = op.getIn().getDefiningOp<ShiftOp>();
    auto resultTy = dyn_cast<IntegerType>(op.getResult().getType());
    if (!shift || !resultTy || !shift.getResult().hasOneUse() ||
        shift->hasAttr("pyc.name") || shift->hasAttr("pyc.debug_keep"))
      return failure();
    auto inputTy = dyn_cast<IntegerType>(shift.getIn().getType());
    if (!inputTy || shift.getResult().getType() != inputTy)
      return failure();
    uint64_t width = inputTy.getWidth();
    uint64_t amount = shift.getAmount();
    uint64_t first = op.getLsb();
    uint64_t end = first + resultTy.getWidth();
    if (!amount || end > width)
      return failure();
    auto slice = [&](uint64_t lsb, uint64_t count) -> Value {
      if (lsb == 0 && count == width)
        return shift.getIn();
      auto type = IntegerType::get(op.getContext(), count);
      return rewriter.create<pyc::ExtractOp>(
          op.getLoc(), type, shift.getIn(), static_cast<int64_t>(lsb),
          IntegerAttr{});
    };
    auto zero = [&](uint64_t count) -> Value {
      auto type = IntegerType::get(op.getContext(), count);
      return constInt(rewriter, op.getLoc(), type,
                      llvm::APInt(static_cast<unsigned>(count), 0));
    };
    Value replacement;
    if constexpr (std::is_same_v<ShiftOp, pyc::ShliOp>) {
      if (amount >= end) {
        replacement = zero(resultTy.getWidth());
      } else if (amount <= first) {
        replacement = slice(first - amount, resultTy.getWidth());
      } else {
        uint64_t dataWidth = end - amount;
        Value data = slice(0, dataWidth);
        Value fill = zero(amount - first);
        SmallVector<Value> parts{data, fill};
        replacement =
            rewriter.create<pyc::ConcatOp>(op.getLoc(), resultTy, parts);
      }
    } else if constexpr (std::is_same_v<ShiftOp, pyc::LshriOp>) {
      if (amount >= width - first) {
        replacement = zero(resultTy.getWidth());
      } else if (amount <= width - end) {
        replacement = slice(first + amount, resultTy.getWidth());
      } else {
        uint64_t dataWidth = width - first - amount;
        Value data = slice(first + amount, dataWidth);
        Value fill = zero(resultTy.getWidth() - dataWidth);
        SmallVector<Value> parts{fill, data};
        replacement =
            rewriter.create<pyc::ConcatOp>(op.getLoc(), resultTy, parts);
      }
    } else {
      if (amount <= width - end) {
        replacement = slice(first + amount, resultTy.getWidth());
      } else {
        uint64_t dataWidth = amount >= width - first
                                 ? 1 : width - first - amount;
        uint64_t dataFirst = width - dataWidth;
        Value data = slice(dataFirst, dataWidth);
        if (dataWidth == resultTy.getWidth())
          replacement = data;
        else
          replacement =
              rewriter.create<pyc::SextOp>(op.getLoc(), resultTy, data);
      }
    }
    rewriter.replaceOp(op, replacement);
    return success();
  }
};

// A scalar constant shift amount shared by several slice readers can use the
// fixed-shift multi-reader rewrite in both backends. Single-reader dynamic
// shifts remain available for graph-level demand analysis.
template <typename DynamicOp, typename ImmediateOp>
struct ConstantDynamicShift : public OpRewritePattern<DynamicOp> {
  using OpRewritePattern<DynamicOp>::OpRewritePattern;

  LogicalResult matchAndRewrite(DynamicOp op,
                                PatternRewriter &rewriter) const override {
    if (op->hasAttr("pyc.name") || op->hasAttr("pyc.debug_keep") ||
        op.getResult().hasOneUse())
      return failure();
    auto amount = getConstAPInt(op.getAmount());
    if (!amount || !amount->isIntN(63))
      return failure();
    rewriter.replaceOpWithNewOp<ImmediateOp>(
        op, op.getResult().getType(), op.getIn(),
        static_cast<int64_t>(amount->getZExtValue()));
    return success();
  }
};

// All slice readers of a fixed shift need only the prefix ending at the
// highest observed bit. A right-shifted prefix is an input extract with
// zero/sign fill; a left shift can run at the narrower modular width.
template <typename ShiftOp>
struct NarrowMultiReaderImmediateShift : public OpRewritePattern<ShiftOp> {
  explicit NarrowMultiReaderImmediateShift(MLIRContext *context)
      : OpRewritePattern<ShiftOp>(context, /*benefit=*/2) {}

  LogicalResult matchAndRewrite(ShiftOp op,
                                PatternRewriter &rewriter) const override {
    if (op->hasAttr("pyc.name") || op->hasAttr("pyc.debug_keep") ||
        op.getResult().hasOneUse())
      return failure();
    auto type = dyn_cast<IntegerType>(op.getResult().getType());
    if (!type || op.getIn().getType() != type)
      return failure();
    SmallVector<pyc::ExtractOp> extracts;
    SmallVector<pyc::TruncOp> truncs;
    uint64_t neededWidth = 0;
    for (OpOperand &use : op.getResult().getUses()) {
      if (auto extract = dyn_cast<pyc::ExtractOp>(use.getOwner())) {
        auto resultTy = dyn_cast<IntegerType>(extract.getResult().getType());
        if (!resultTy || extract.getIn() != op.getResult() ||
            extract->hasAttr("pyc.name") ||
            extract->hasAttr("pyc.debug_keep"))
          return failure();
        neededWidth = std::max<uint64_t>(
            neededWidth, static_cast<uint64_t>(extract.getLsb()) +
                             resultTy.getWidth());
        extracts.push_back(extract);
      } else if (auto trunc = dyn_cast<pyc::TruncOp>(use.getOwner())) {
        auto resultTy = dyn_cast<IntegerType>(trunc.getResult().getType());
        if (!resultTy || trunc.getIn() != op.getResult() ||
            trunc->hasAttr("pyc.name") ||
            trunc->hasAttr("pyc.debug_keep"))
          return failure();
        neededWidth = std::max<uint64_t>(neededWidth, resultTy.getWidth());
        truncs.push_back(trunc);
      } else {
        return failure();
      }
    }
    uint64_t width = type.getWidth();
    uint64_t amount = op.getAmount();
    if (!neededWidth || neededWidth >= width)
      return failure();
    Type narrowType = IntegerType::get(op.getContext(), neededWidth);
    rewriter.setInsertionPoint(op);
    Value narrowed;
    if constexpr (std::is_same_v<ShiftOp, pyc::ShliOp>) {
      if (amount >= neededWidth) {
        narrowed = constInt(rewriter, op.getLoc(), narrowType,
                            llvm::APInt(neededWidth, 0));
      } else {
        Value input = rewriter.create<pyc::TruncOp>(
            op.getLoc(), narrowType, op.getIn());
        narrowed = rewriter.create<pyc::ShliOp>(
            op.getLoc(), narrowType, input, op.getAmount());
      }
    } else {
      uint64_t dataWidth = amount >= width
                               ? 0
                               : std::min<uint64_t>(neededWidth,
                                                    width - amount);
      if (dataWidth == 0) {
        if constexpr (std::is_same_v<ShiftOp, pyc::LshriOp>) {
          narrowed = constInt(rewriter, op.getLoc(), narrowType,
                              llvm::APInt(neededWidth, 0));
        } else {
          Type bitType = IntegerType::get(op.getContext(), 1);
          Value sign = rewriter.create<pyc::ExtractOp>(
              op.getLoc(), bitType, op.getIn(), width - 1,
              IntegerAttr{});
          narrowed = rewriter.create<pyc::SextOp>(
              op.getLoc(), narrowType, sign);
        }
      } else {
        Type dataType = IntegerType::get(op.getContext(), dataWidth);
        Value data = rewriter.create<pyc::ExtractOp>(
            op.getLoc(), dataType, op.getIn(), amount, IntegerAttr{});
        if (dataWidth == neededWidth) {
          narrowed = data;
        } else if constexpr (std::is_same_v<ShiftOp, pyc::LshriOp>) {
          narrowed = rewriter.create<pyc::ZextOp>(
              op.getLoc(), narrowType, data);
        } else {
          narrowed = rewriter.create<pyc::SextOp>(
              op.getLoc(), narrowType, data);
        }
      }
    }
    for (pyc::ExtractOp extract : extracts) {
      Type resultType = extract.getResult().getType();
      Value result = resultType == narrowType && extract.getLsb() == 0
                         ? narrowed
                         : rewriter.create<pyc::ExtractOp>(
                               extract.getLoc(), resultType, narrowed,
                               extract.getLsb(), extract.getMsbAttr())
                               .getResult();
      rewriter.replaceOp(extract, result);
    }
    for (pyc::TruncOp trunc : truncs) {
      Type resultType = trunc.getResult().getType();
      Value result = resultType == narrowType
                         ? narrowed
                         : rewriter.create<pyc::TruncOp>(
                               trunc.getLoc(), resultType, narrowed)
                               .getResult();
      rewriter.replaceOp(trunc, result);
    }
    rewriter.eraseOp(op);
    return success();
  }
};

// The low K bits of modular add/sub/mul depend only on the low K operand bits.
// Keep the carry path below a requested slice while discarding unused upper
// arithmetic. This is a hardware value rewrite shared by both backends.
template <typename ArithmeticOp>
struct NarrowMultiReaderArithmetic : public OpRewritePattern<ArithmeticOp> {
  explicit NarrowMultiReaderArithmetic(MLIRContext *context)
      : OpRewritePattern<ArithmeticOp>(context, /*benefit=*/2) {}

  LogicalResult matchAndRewrite(ArithmeticOp op,
                                PatternRewriter &rewriter) const override {
    if (op->hasAttr("pyc.name") || op->hasAttr("pyc.debug_keep") ||
        op.getResult().hasOneUse())
      return failure();
    auto originalTy = dyn_cast<IntegerType>(op.getResult().getType());
    if (!originalTy || op.getLhs().getType() != originalTy ||
        op.getRhs().getType() != originalTy)
      return failure();
    SmallVector<pyc::ExtractOp> extracts;
    SmallVector<pyc::TruncOp> truncs;
    uint64_t neededWidth = 0;
    for (OpOperand &use : op.getResult().getUses()) {
      if (auto extract = dyn_cast<pyc::ExtractOp>(use.getOwner())) {
        auto resultTy = dyn_cast<IntegerType>(extract.getResult().getType());
        if (!resultTy || extract.getIn() != op.getResult() ||
            extract->hasAttr("pyc.name") ||
            extract->hasAttr("pyc.debug_keep"))
          return failure();
        neededWidth = std::max<uint64_t>(
            neededWidth, static_cast<uint64_t>(extract.getLsb()) +
                             resultTy.getWidth());
        extracts.push_back(extract);
      } else if (auto trunc = dyn_cast<pyc::TruncOp>(use.getOwner())) {
        auto resultTy = dyn_cast<IntegerType>(trunc.getResult().getType());
        if (!resultTy || trunc.getIn() != op.getResult() ||
            trunc->hasAttr("pyc.name") ||
            trunc->hasAttr("pyc.debug_keep"))
          return failure();
        neededWidth = std::max<uint64_t>(neededWidth, resultTy.getWidth());
        truncs.push_back(trunc);
      } else {
        return failure();
      }
    }
    if (neededWidth == 0 || neededWidth >= originalTy.getWidth())
      return failure();
    Type narrowTy = IntegerType::get(op.getContext(), neededWidth);
    rewriter.setInsertionPoint(op);
    Value lhs = rewriter.create<pyc::TruncOp>(
        op.getLoc(), narrowTy, op.getLhs());
    Value rhs = rewriter.create<pyc::TruncOp>(
        op.getLoc(), narrowTy, op.getRhs());
    Value narrowed = rewriter.create<ArithmeticOp>(
        op.getLoc(), narrowTy, lhs, rhs);
    for (pyc::ExtractOp extract : extracts) {
      Type resultTy = extract.getResult().getType();
      Value result = resultTy == narrowTy && extract.getLsb() == 0
                         ? narrowed
                         : rewriter.create<pyc::ExtractOp>(
                               extract.getLoc(), resultTy, narrowed,
                               extract.getLsb(), extract.getMsbAttr())
                               .getResult();
      rewriter.replaceOp(extract, result);
    }
    for (pyc::TruncOp trunc : truncs) {
      Type resultTy = trunc.getResult().getType();
      Value result = resultTy == narrowTy
                         ? narrowed
                         : rewriter.create<pyc::TruncOp>(
                               trunc.getLoc(), resultTy, narrowed)
                               .getResult();
      rewriter.replaceOp(trunc, result);
    }
    rewriter.eraseOp(op);
    return success();
  }
};

template <typename ArithmeticOp>
struct ExtractThroughArithmetic : public OpRewritePattern<pyc::ExtractOp> {
  using OpRewritePattern<pyc::ExtractOp>::OpRewritePattern;

  LogicalResult matchAndRewrite(pyc::ExtractOp op,
                                PatternRewriter &rewriter) const override {
    if (op->hasAttr("pyc.name") || op->hasAttr("pyc.debug_keep"))
      return failure();
    auto arithmetic = op.getIn().getDefiningOp<ArithmeticOp>();
    auto resultTy = dyn_cast<IntegerType>(op.getResult().getType());
    if (!arithmetic || !resultTy || !arithmetic.getResult().hasOneUse() ||
        arithmetic->hasAttr("pyc.name") ||
        arithmetic->hasAttr("pyc.debug_keep"))
      return failure();
    auto originalTy = dyn_cast<IntegerType>(arithmetic.getResult().getType());
    if (!originalTy || arithmetic.getLhs().getType() != originalTy ||
        arithmetic.getRhs().getType() != originalTy)
      return failure();
    uint64_t upperWidth = static_cast<uint64_t>(op.getLsb()) +
                          resultTy.getWidth();
    if (upperWidth == 0 || upperWidth >= originalTy.getWidth())
      return failure();
    auto narrowTy = IntegerType::get(op.getContext(), upperWidth);
    Value lhs = rewriter.create<pyc::TruncOp>(op.getLoc(), narrowTy,
                                               arithmetic.getLhs());
    Value rhs = rewriter.create<pyc::TruncOp>(op.getLoc(), narrowTy,
                                               arithmetic.getRhs());
    Value narrowed = rewriter.create<ArithmeticOp>(op.getLoc(), narrowTy,
                                                    lhs, rhs);
    if (op.getLsb() == 0)
      rewriter.replaceOp(op, narrowed);
    else
      rewriter.replaceOpWithNewOp<pyc::ExtractOp>(
          op, resultTy, narrowed, op.getLsb(), op.getMsbAttr());
    return success();
  }
};

// A truncation is a low-bit observation. Modular arithmetic above that width
// cannot affect the result, so perform the operation at the observed width.
template <typename ArithmeticOp>
struct TruncThroughArithmetic : public OpRewritePattern<pyc::TruncOp> {
  using OpRewritePattern<pyc::TruncOp>::OpRewritePattern;

  LogicalResult matchAndRewrite(pyc::TruncOp op,
                                PatternRewriter &rewriter) const override {
    if (op->hasAttr("pyc.name") || op->hasAttr("pyc.debug_keep"))
      return failure();
    auto arithmetic = op.getIn().getDefiningOp<ArithmeticOp>();
    auto resultTy = dyn_cast<IntegerType>(op.getResult().getType());
    if (!arithmetic || !resultTy || !arithmetic.getResult().hasOneUse() ||
        arithmetic->hasAttr("pyc.name") ||
        arithmetic->hasAttr("pyc.debug_keep"))
      return failure();
    auto originalTy = dyn_cast<IntegerType>(arithmetic.getResult().getType());
    if (!originalTy || resultTy.getWidth() >= originalTy.getWidth() ||
        arithmetic.getLhs().getType() != originalTy ||
        arithmetic.getRhs().getType() != originalTy)
      return failure();
    Value lhs = rewriter.create<pyc::TruncOp>(
        op.getLoc(), resultTy, arithmetic.getLhs());
    Value rhs = rewriter.create<pyc::TruncOp>(
        op.getLoc(), resultTy, arithmetic.getRhs());
    rewriter.replaceOpWithNewOp<ArithmeticOp>(op, resultTy, lhs, rhs);
    return success();
  }
};

struct MuxI1ToLogic : public OpRewritePattern<pyc::MuxOp> {
  using OpRewritePattern::OpRewritePattern;

  LogicalResult matchAndRewrite(pyc::MuxOp op, PatternRewriter &rewriter) const override {
    if (!isI1OrI1Vector(op.getType()))
      return failure();

    auto aConst = getConstUInt(op.getA());
    auto bConst = getConstUInt(op.getB());

    if (aConst && bConst) {
      uint64_t a = (*aConst) & 1;
      uint64_t b = (*bConst) & 1;
      if (a == b) {
        rewriter.replaceOp(op, a ? op.getA() : op.getB());
        return success();
      }
      if (a == 1 && b == 0) {
        rewriter.replaceOp(op, op.getSel());
        return success();
      }
      if (a == 0 && b == 1) {
        rewriter.replaceOpWithNewOp<pyc::NotOp>(op, op.getSel());
        return success();
      }
      return failure();
    }

    // For i1, convert muxes with constant arms into simple gates.
    if (bConst && (((*bConst) & 1) == 0)) {
      // sel ? a : 0  ==>  sel & a
      rewriter.replaceOpWithNewOp<pyc::AndOp>(op, op.getResult().getType(), op.getSel(), op.getA());
      return success();
    }
    if (aConst && (((*aConst) & 1) == 1)) {
      // sel ? 1 : b  ==>  sel | b
      rewriter.replaceOpWithNewOp<pyc::OrOp>(op, op.getResult().getType(), op.getSel(), op.getB());
      return success();
    }
    if (bConst && (((*bConst) & 1) == 1)) {
      // sel ? a : 1  ==>  (~sel) | a
      Value nsel = rewriter.create<pyc::NotOp>(op.getLoc(), op.getSel());
      rewriter.replaceOpWithNewOp<pyc::OrOp>(op, op.getResult().getType(), nsel, op.getA());
      return success();
    }
    if (aConst && (((*aConst) & 1) == 0)) {
      // sel ? 0 : b  ==>  (~sel) & b
      Value nsel = rewriter.create<pyc::NotOp>(op.getLoc(), op.getSel());
      rewriter.replaceOpWithNewOp<pyc::AndOp>(op, op.getResult().getType(), nsel, op.getB());
      return success();
    }

    return failure();
  }
};

struct AndBasicSimplify : public OpRewritePattern<pyc::AndOp> {
  using OpRewritePattern::OpRewritePattern;

  LogicalResult matchAndRewrite(pyc::AndOp op, PatternRewriter &rewriter) const override {
    Value lhs = stripAlias(op.getLhs());
    Value rhs = stripAlias(op.getRhs());

    // a & a ==> a
    if (lhs == rhs) {
      rewriter.replaceOp(op, op.getLhs());
      return success();
    }

    // a & ~a ==> 0
    auto [lb, ln] = stripNot(lhs);
    auto [rb, rn] = stripNot(rhs);
    if ((lb == rb) && (ln != rn)) {
      Value z = constInt(rewriter, op.getLoc(), op.getType(), llvm::APInt(1, 0));
      if (!z)
        return failure();
      rewriter.replaceOp(op, z);
      return success();
    }

    return failure();
  }
};

struct OrBasicSimplify : public OpRewritePattern<pyc::OrOp> {
  using OpRewritePattern::OpRewritePattern;

  LogicalResult matchAndRewrite(pyc::OrOp op, PatternRewriter &rewriter) const override {
    Value lhs = stripAlias(op.getLhs());
    Value rhs = stripAlias(op.getRhs());

    // a | a ==> a
    if (lhs == rhs) {
      rewriter.replaceOp(op, op.getLhs());
      return success();
    }

    // a | ~a ==> all-ones
    auto [lb, ln] = stripNot(lhs);
    auto [rb, rn] = stripNot(rhs);
    if ((lb == rb) && (ln != rn)) {
      Value ones = constInt(rewriter, op.getLoc(), op.getType(), llvm::APInt::getAllOnes(
          isa<IntegerType>(op.getType()) ? cast<IntegerType>(op.getType()).getWidth() : 1));
      if (!ones)
        return failure();
      rewriter.replaceOp(op, ones);
      return success();
    }

    return failure();
  }
};

struct OrAndXorFactor : public OpRewritePattern<pyc::OrOp> {
  using OpRewritePattern::OpRewritePattern;

  LogicalResult matchAndRewrite(pyc::OrOp op, PatternRewriter &rewriter) const override {
    Value lhs = stripAlias(op.getLhs());
    Value rhs = stripAlias(op.getRhs());
    auto a0 = lhs.getDefiningOp<pyc::AndOp>();
    auto a1 = rhs.getDefiningOp<pyc::AndOp>();
    if (!a0 || !a1)
      return failure();

    // Try candidates based on the first AND's operands.
    auto [x0, _x0n] = stripNot(a0.getLhs());
    auto [x1, _x1n] = stripNot(a0.getRhs());
    llvm::SmallVector<std::pair<Value, Value>, 2> cands;
    cands.push_back({x0, x1});
    if (x0 != x1)
      cands.push_back({x1, x0});

    for (auto [A, B] : cands) {
      // XOR: (A & ~B) | (~A & B) ==> A ^ B
      if (andMatches(a0, A, /*aNot=*/false, B, /*bNot=*/true) && andMatches(a1, A, /*aNot=*/true, B, /*bNot=*/false)) {
        rewriter.replaceOpWithNewOp<pyc::XorOp>(op, op.getType(), A, B);
        return success();
      }
      if (andMatches(a0, A, /*aNot=*/true, B, /*bNot=*/false) && andMatches(a1, A, /*aNot=*/false, B, /*bNot=*/true)) {
        rewriter.replaceOpWithNewOp<pyc::XorOp>(op, op.getType(), A, B);
        return success();
      }

      // XNOR: (A & B) | (~A & ~B) ==> ~(A ^ B)
      if (andMatches(a0, A, /*aNot=*/false, B, /*bNot=*/false) && andMatches(a1, A, /*aNot=*/true, B, /*bNot=*/true)) {
        Value x = rewriter.create<pyc::XorOp>(op.getLoc(), op.getType(), A, B);
        rewriter.replaceOpWithNewOp<pyc::NotOp>(op, op.getType(), x);
        return success();
      }
      if (andMatches(a0, A, /*aNot=*/true, B, /*bNot=*/true) && andMatches(a1, A, /*aNot=*/false, B, /*bNot=*/false)) {
        Value x = rewriter.create<pyc::XorOp>(op.getLoc(), op.getType(), A, B);
        rewriter.replaceOpWithNewOp<pyc::NotOp>(op, op.getType(), x);
        return success();
      }
    }

    return failure();
  }
};

struct VGetOfVCreate : public OpRewritePattern<pyc::VGetOp> {
  using OpRewritePattern::OpRewritePattern;

  LogicalResult matchAndRewrite(pyc::VGetOp op, PatternRewriter &rewriter) const override {
    auto create = op.getVec().getDefiningOp<pyc::VCreateOp>();
    if (!create)
      return failure();
    std::uint64_t idx = op.getIndex();
    if (idx >= create.getElements().size())
      return failure();
    rewriter.replaceOp(op, *std::next(create.getElements().begin(), static_cast<long>(idx)));
    return success();
  }
};

struct VGetOfVBroadcast : public OpRewritePattern<pyc::VGetOp> {
  using OpRewritePattern::OpRewritePattern;

  LogicalResult matchAndRewrite(pyc::VGetOp op, PatternRewriter &rewriter) const override {
    auto broadcast = op.getVec().getDefiningOp<pyc::VBroadcastOp>();
    if (!broadcast)
      return failure();
    rewriter.replaceOp(op, broadcast.getScalar());
    return success();
  }
};

struct VGetOfVBroadcastDim : public OpRewritePattern<pyc::VGetOp> {
  using OpRewritePattern::OpRewritePattern;

  LogicalResult matchAndRewrite(pyc::VGetOp get,
                                PatternRewriter &rewriter) const override {
    auto broadcast = get.getVec().getDefiningOp<pyc::VBroadcastDimOp>();
    if (!broadcast || get->hasAttr("pyc.name") ||
        get->hasAttr("pyc.debug_keep") ||
        broadcast->hasAttr("pyc.name") ||
        broadcast->hasAttr("pyc.debug_keep"))
      return failure();
    auto sourceType = dyn_cast<VectorType>(broadcast.getVec().getType());
    auto broadcastType = dyn_cast<VectorType>(get.getVec().getType());
    if (!sourceType || !broadcastType ||
        get.getIndex() >= static_cast<uint64_t>(broadcastType.getDimSize(0)))
      return failure();
    int64_t dim = broadcast.getDim();
    if (dim == 0) {
      if (get.getResult().getType() != broadcast.getVec().getType())
        return failure();
      rewriter.replaceOp(get, broadcast.getVec());
      return success();
    }
    Type laneType = vectorLaneType(sourceType);
    Value lane = rewriter.create<pyc::VGetOp>(
        get.getLoc(), laneType, broadcast.getVec(), get.getIndexAttr());
    Type resultType = get.getResult().getType();
    if (sourceType.getRank() == 1) {
      rewriter.replaceOpWithNewOp<pyc::VBroadcastOp>(
          get, resultType, lane, broadcast.getSize());
    } else {
      OperationState state(get.getLoc(), broadcast->getName());
      state.addOperands(lane);
      state.addTypes(resultType);
      state.addAttribute("size", broadcast.getSizeAttr());
      state.addAttribute("dim", rewriter.getI64IntegerAttr(dim - 1));
      rewriter.replaceOp(get, rewriter.create(state)->getResult(0));
    }
    return success();
  }
};

// A fixed read of a rank-2 reduction needs only the selected row or column.
// Rewrite all readers together so each selected lane is reduced once and the
// original aggregate reduction can be removed before either backend runs.
template <typename ReduceOp>
struct VGetOfRank2Reduce : public OpRewritePattern<pyc::VGetOp> {
  using OpRewritePattern<pyc::VGetOp>::OpRewritePattern;

  LogicalResult matchAndRewrite(pyc::VGetOp get,
                                PatternRewriter &rewriter) const override {
    auto reduce = get.getVec().getDefiningOp<ReduceOp>();
    if (!reduce || !reduce.getDim() ||
        reduce->hasAttr("pyc.name") || reduce->hasAttr("pyc.debug_keep"))
      return failure();
    auto inputType = dyn_cast<VectorType>(reduce.getVec().getType());
    auto resultType = dyn_cast<VectorType>(reduce.getResult().getType());
    if (!inputType || inputType.getRank() != 2 || !resultType ||
        resultType.getRank() != 1 ||
        (reduce.getDim() != 0 && reduce.getDim() != 1))
      return failure();
    std::map<uint64_t, SmallVector<pyc::VGetOp>> readers;
    for (OpOperand &use : reduce.getResult().getUses()) {
      auto reader = dyn_cast<pyc::VGetOp>(use.getOwner());
      if (!reader || reader.getVec() != reduce.getResult() ||
          reader->hasAttr("pyc.name") ||
          reader->hasAttr("pyc.debug_keep") ||
          reader.getIndex() >=
              static_cast<uint64_t>(resultType.getDimSize(0)))
        return failure();
      readers[reader.getIndex()].push_back(reader);
    }
    if (readers.empty() ||
        readers.size() >= static_cast<size_t>(resultType.getDimSize(0)))
      return failure();
    rewriter.setInsertionPoint(reduce);
    Type scalarType = inputType.getElementType();
    Type rowType = VectorType::get({inputType.getDimSize(1)}, scalarType);
    Type columnType = VectorType::get({inputType.getDimSize(0)}, scalarType);
    SmallVector<Value> sourceRows;
    if (*reduce.getDim() == 0) {
      sourceRows.reserve(inputType.getDimSize(0));
      for (int64_t row = 0; row < inputType.getDimSize(0); ++row)
        sourceRows.push_back(rewriter.create<pyc::VGetOp>(
            get.getLoc(), rowType, reduce.getVec(),
            rewriter.getI64IntegerAttr(row)));
    }
    for (auto &[index, uses] : readers) {
      Value selected;
      if (*reduce.getDim() == 1) {
        selected = rewriter.create<pyc::VGetOp>(
            uses.front().getLoc(), rowType, reduce.getVec(),
            rewriter.getI64IntegerAttr(index));
      } else {
        SmallVector<Value> columns;
        columns.reserve(inputType.getDimSize(0));
        for (Value rowValue : sourceRows) {
          columns.push_back(rewriter.create<pyc::VGetOp>(
              uses.front().getLoc(), scalarType, rowValue,
              rewriter.getI64IntegerAttr(index)));
        }
        selected = rewriter.create<pyc::VCreateOp>(
            uses.front().getLoc(), columnType, columns);
      }
      OperationState state(uses.front().getLoc(), reduce->getName());
      state.addOperands(selected);
      state.addTypes(scalarType);
      state.addAttribute("mode", reduce.getModeAttr());
      Value lane = rewriter.create(state)->getResult(0);
      for (pyc::VGetOp reader : uses)
        rewriter.replaceOp(reader, lane);
    }
    rewriter.eraseOp(reduce);
    return success();
  }
};

struct VGetOfVectorMux : public OpRewritePattern<pyc::VGetOp> {
  using OpRewritePattern::OpRewritePattern;

  LogicalResult matchAndRewrite(pyc::VGetOp op, PatternRewriter &rewriter) const override {
    auto mux = op.getVec().getDefiningOp<pyc::MuxOp>();
    if (!mux || !mux.getResult().hasOneUse())
      return failure();
    auto muxVT = dyn_cast<VectorType>(mux.getResult().getType());
    if (!muxVT)
      return failure();
    std::uint64_t idx = op.getIndex();
    if (idx >= static_cast<std::uint64_t>(muxVT.getDimSize(0)))
      return failure();

    Value sel = mux.getSel();
    if (auto selVT = dyn_cast<VectorType>(sel.getType())) {
      Type selLaneTy = isa<VectorType>(selVT.getElementType())
                           ? Type(selVT.getElementType())
                           : (selVT.getRank() == 1
                                  ? Type(selVT.getElementType())
                                  : Type(VectorType::get(
                                        selVT.getShape().drop_front(),
                                        selVT.getElementType())));
      sel = rewriter.create<pyc::VGetOp>(op.getLoc(), selLaneTy, sel, idx);
    }

    Type laneTy = op.getResult().getType();
    Value a = rewriter.create<pyc::VGetOp>(op.getLoc(), laneTy, mux.getA(), idx);
    Value b = rewriter.create<pyc::VGetOp>(op.getLoc(), laneTy, mux.getB(), idx);
    rewriter.replaceOpWithNewOp<pyc::MuxOp>(op, laneTy, sel, a, b);
    return success();
  }
};

struct VCreateOfConsecutiveVGets : public OpRewritePattern<pyc::VCreateOp> {
  using OpRewritePattern::OpRewritePattern;

  LogicalResult matchAndRewrite(pyc::VCreateOp op, PatternRewriter &rewriter) const override {
    if (op.getElements().empty())
      return failure();

    Value src;
    std::uint64_t expected = 0;
    for (Value elem : op.getElements()) {
      auto get = elem.getDefiningOp<pyc::VGetOp>();
      if (!get || get.getIndex() != expected++)
        return failure();
      if (!src)
        src = get.getVec();
      else if (src != get.getVec())
        return failure();
    }

    if (!src || src.getType() != op.getResult().getType())
      return failure();
    rewriter.replaceOp(op, src);
    return success();
  }
};

template <typename ReduceOp>
struct VReduceOfVBroadcast : public OpRewritePattern<ReduceOp> {
  using OpRewritePattern<ReduceOp>::OpRewritePattern;

  LogicalResult matchAndRewrite(ReduceOp op, PatternRewriter &rewriter) const override {
    auto broadcast = op.getVec().template getDefiningOp<pyc::VBroadcastOp>();
    if (!broadcast)
      return failure();
    auto vecTy = dyn_cast<VectorType>(op.getVec().getType());
    if (!vecTy || vecTy.getRank() != 1)
      return failure();
    if (op.getResult().getType() != broadcast.getScalar().getType())
      return failure();
    rewriter.replaceOp(op, broadcast.getScalar());
    return success();
  }
};

template <typename ReduceOp>
struct VReduceSingleLaneDim : public OpRewritePattern<ReduceOp> {
  using OpRewritePattern<ReduceOp>::OpRewritePattern;

  LogicalResult matchAndRewrite(ReduceOp op, PatternRewriter &rewriter) const override {
    auto vecTy = dyn_cast<VectorType>(op.getVec().getType());
    if (!vecTy)
      return failure();
    if (!op.getDim()) {
      for (int64_t extent : vecTy.getShape())
        if (extent != 1)
          return failure();
      if (vecTy.getRank() == 1) {
        rewriter.replaceOpWithNewOp<pyc::VGetOp>(op, op.getResult().getType(), op.getVec(), 0);
        return success();
      }
      auto rowTy = dyn_cast<VectorType>(vecTy.getElementType());
      if (!rowTy)
        return failure();
      auto row = rewriter.create<pyc::VGetOp>(op.getLoc(), rowTy, op.getVec(), 0);
      rewriter.replaceOpWithNewOp<pyc::VGetOp>(op, op.getResult().getType(), row, 0);
      return success();
    }
    std::int64_t dim = op.getDim().value_or(0);
    if (dim < 0 || dim >= vecTy.getRank() || vecTy.getDimSize(dim) != 1)
      return failure();

    if (dim == 0) {
      rewriter.replaceOpWithNewOp<pyc::VGetOp>(op, op.getResult().getType(), op.getVec(), 0);
      return success();
    }

    // For rank-2 vectors represented as v_create(row0, row1, ...), reducing a
    // single-column inner dimension picks lane 0 from every row.
    auto create = op.getVec().template getDefiningOp<pyc::VCreateOp>();
    if (!create)
      return failure();
    SmallVector<Value> elems;
    elems.reserve(create.getElements().size());
    for (Value row : create.getElements()) {
      auto rowTy = dyn_cast<VectorType>(row.getType());
      if (!rowTy || rowTy.getRank() != 1 || rowTy.getDimSize(0) != 1)
        return failure();
      elems.push_back(rewriter.create<pyc::VGetOp>(op.getLoc(), rowTy.getElementType(), row, 0));
    }
    rewriter.replaceOpWithNewOp<pyc::VCreateOp>(op, op.getResult().getType(), elems);
    return success();
  }
};

struct CombCanonicalizePass : public PassWrapper<CombCanonicalizePass, OperationPass<func::FuncOp>> {
  MLIR_DEFINE_EXPLICIT_INTERNAL_INLINE_TYPE_ID(CombCanonicalizePass)

  StringRef getArgument() const override { return "pyc-comb-canonicalize"; }
  StringRef getDescription() const override {
    return "Simplify combinational PYC logic (mux canonicalization and small boolean rewrites)";
  }

  void runOnOperation() override {
    func::FuncOp f = getOperation();
    RewritePatternSet patterns(f.getContext());
    patterns.add<MuxSameSelSimplify,
                 EqConcatConstantSplit,
                 EqShiftedOrConstantSplit,
                 ExtractOneHotShift,
                 ExtractThroughConcat,
                 ExtractThroughNot,
                 ExtractThroughWidthCast,
                 ExtractThroughImmediateShift<pyc::ShliOp>,
                 ExtractThroughImmediateShift<pyc::LshriOp>,
                 ExtractThroughImmediateShift<pyc::AshriOp>,
                 ConstantDynamicShift<pyc::ShlOp, pyc::ShliOp>,
                 ConstantDynamicShift<pyc::LshrOp, pyc::LshriOp>,
                 ConstantDynamicShift<pyc::AshrOp, pyc::AshriOp>,
                 NarrowMultiReaderImmediateShift<pyc::ShliOp>,
                 NarrowMultiReaderImmediateShift<pyc::LshriOp>,
                 NarrowMultiReaderImmediateShift<pyc::AshriOp>,
                 SplitBitwiseBySlices<pyc::AndOp>,
                 SplitBitwiseBySlices<pyc::OrOp>,
                 SplitBitwiseBySlices<pyc::XorOp>,
                 SplitBitwiseBySlices<pyc::NotOp>,
                 SplitMuxBySlices,
                 ExtractThroughBitwise<pyc::AndOp>,
                 ExtractThroughBitwise<pyc::OrOp>,
                 ExtractThroughBitwise<pyc::XorOp>,
                 NarrowMultiReaderMux,
                 ExtractThroughMux,
                 NarrowMultiReaderArithmetic<pyc::AddOp>,
                 NarrowMultiReaderArithmetic<pyc::SubOp>,
                 NarrowMultiReaderArithmetic<pyc::MulOp>,
                 ExtractThroughArithmetic<pyc::AddOp>,
                 ExtractThroughArithmetic<pyc::SubOp>,
                 ExtractThroughArithmetic<pyc::MulOp>,
                 TruncThroughArithmetic<pyc::AddOp>,
                 TruncThroughArithmetic<pyc::SubOp>,
                 TruncThroughArithmetic<pyc::MulOp>,
                 SplitRegisterBySlices,
                 SplitVectorRegisterLanes,
                 VGetThroughElementwise<pyc::AddOp>,
                 VGetThroughElementwise<pyc::SubOp>,
                 VGetThroughElementwise<pyc::MulOp>,
                 VGetThroughElementwise<pyc::UdivOp>,
                 VGetThroughElementwise<pyc::UremOp>,
                 VGetThroughElementwise<pyc::SdivOp>,
                 VGetThroughElementwise<pyc::SremOp>,
                 VGetThroughElementwise<pyc::AndOp>,
                 VGetThroughElementwise<pyc::OrOp>,
                 VGetThroughElementwise<pyc::XorOp>,
                 VGetThroughElementwise<pyc::NotOp>,
                 VGetThroughElementwise<pyc::MuxOp>,
                 VGetThroughElementwise<pyc::EqOp>,
                 VGetThroughElementwise<pyc::UltOp>,
                 VGetThroughElementwise<pyc::SltOp>,
                 VGetThroughElementwise<pyc::TruncOp>,
                 VGetThroughElementwise<pyc::ZextOp>,
                 VGetThroughElementwise<pyc::SextOp>,
                 VGetThroughElementwise<pyc::ExtractOp>,
                 VGetThroughElementwise<pyc::ShliOp>,
                 VGetThroughElementwise<pyc::LshriOp>,
                 VGetThroughElementwise<pyc::AshriOp>,
                 VGetThroughElementwise<pyc::ShlOp>,
                 VGetThroughElementwise<pyc::LshrOp>,
                 VGetThroughElementwise<pyc::AshrOp>,
                 VGetThroughElementwise<arith::SelectOp>,
                 MuxI1ToLogic,
                 AndBasicSimplify,
                 OrBasicSimplify,
                 OrAndXorFactor,
                 VGetOfVCreate,
                 VGetOfVBroadcast,
                 VGetOfVBroadcastDim,
                 VGetOfRank2Reduce<pyc::VOrReduceOp>,
                 VGetOfRank2Reduce<pyc::VAndReduceOp>,
                 VGetOfRank2Reduce<pyc::VAddReduceOp>,
                 VGetOfVectorMux,
                 VCreateOfConsecutiveVGets,
                 FoldVectorConstantLanes<pyc::AddOp>,
                 FoldVectorConstantLanes<pyc::SubOp>,
                 FoldVectorConstantLanes<pyc::MulOp>,
                 FoldVectorConstantLanes<pyc::UdivOp>,
                 FoldVectorConstantLanes<pyc::UremOp>,
                 FoldVectorConstantLanes<pyc::SdivOp>,
                 FoldVectorConstantLanes<pyc::SremOp>,
                 FoldVectorConstantLanes<pyc::AndOp>,
                 FoldVectorConstantLanes<pyc::OrOp>,
                 FoldVectorConstantLanes<pyc::XorOp>,
                 FoldVectorConstantLanes<pyc::EqOp>,
                 FoldVectorConstantLanes<pyc::UltOp>,
                 FoldVectorConstantLanes<pyc::SltOp>,
                 FoldRank2ReduceLanes<pyc::VOrReduceOp>,
                 FoldRank2ReduceLanes<pyc::VAndReduceOp>,
                 FoldRank2ReduceLanes<pyc::VAddReduceOp>,
                 VReduceOfVBroadcast<pyc::VOrReduceOp>,
                 VReduceOfVBroadcast<pyc::VAndReduceOp>,
                 VReduceSingleLaneDim<pyc::VOrReduceOp>,
                 VReduceSingleLaneDim<pyc::VAndReduceOp>,
                 VReduceSingleLaneDim<pyc::VAddReduceOp>>(f.getContext());

    GreedyRewriteConfig cfg;
    if (failed(applyPatternsAndFoldGreedily(f, std::move(patterns), cfg)))
      signalPassFailure();
  }
};

} // namespace

std::unique_ptr<::mlir::Pass> createCombCanonicalizePass() { return std::make_unique<CombCanonicalizePass>(); }

static PassRegistration<CombCanonicalizePass> pass;

} // namespace pyc
