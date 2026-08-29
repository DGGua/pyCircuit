#include "pyc/Transforms/CombMemoization.h"

#include "pyc/Dialect/PYC/PYCOps.h"
#include "pyc/Dialect/PYC/PYCTypes.h"

#include "mlir/Dialect/Arith/IR/Arith.h"
#include "mlir/IR/BuiltinTypes.h"

using namespace mlir;

namespace pyc {

bool isMemoizableCombOperation(Operation *op) {
  return isa<pyc::ConstantOp, pyc::AddOp, pyc::SubOp, pyc::MulOp, pyc::UdivOp,
             pyc::UremOp, pyc::SdivOp, pyc::SremOp, pyc::MuxOp, pyc::AndOp,
             pyc::OrOp, pyc::XorOp, pyc::NotOp, pyc::ConcatOp, pyc::AliasOp,
             pyc::ResetActiveOp, pyc::EqOp, pyc::UltOp, pyc::SltOp,
             pyc::TruncOp, pyc::ZextOp, pyc::SextOp, pyc::ExtractOp,
             pyc::ShliOp, pyc::LshriOp, pyc::AshriOp, pyc::ShlOp, pyc::LshrOp,
             pyc::AshrOp, pyc::VGetOp, pyc::VCreateOp, pyc::VBroadcastOp,
             pyc::VBroadcastDimOp, pyc::VOrReduceOp, pyc::VAndReduceOp,
             pyc::VAddReduceOp, arith::SelectOp>(op);
}

bool isMemoizableCombType(Type type) {
  if (isa<pyc::ClockType, pyc::ResetType>(type))
    return true;
  if (auto integer = dyn_cast<IntegerType>(type))
    return integer.getWidth() != 0;
  if (auto vector = dyn_cast<VectorType>(type))
    return vector.getRank() != 0 && vector.hasStaticShape() &&
           isa<IntegerType>(vector.getElementType());
  return false;
}

} // namespace pyc
