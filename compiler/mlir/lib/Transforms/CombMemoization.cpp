#include "pyc/Transforms/CombMemoization.h"

#include "pyc/Dialect/PYC/PYCOps.h"

#include "mlir/Dialect/Arith/IR/Arith.h"

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

} // namespace pyc
