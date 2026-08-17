#include "pyc/Transforms/Passes.h"

#include "pyc/Dialect/PYC/PYCOps.h"
#include "pyc/Transforms/CombMemoization.h"
#include "pyc/Transforms/CombPartition.h"

#include "mlir/Dialect/Func/IR/FuncOps.h"
#include "mlir/IR/BuiltinAttributes.h"
#include "mlir/IR/BuiltinTypes.h"
#include "mlir/Pass/Pass.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringMap.h"

#include <cstdint>
#include <limits>
#include <optional>
#include <string>

using namespace mlir;

namespace pyc {
namespace {

static bool isAlpha(char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

static bool isDigit(char c) { return c >= '0' && c <= '9'; }

static bool isIdentStart(char c) { return isAlpha(c) || c == '_'; }

static bool isIdentChar(char c) { return isIdentStart(c) || isDigit(c); }

static std::string sanitizeIdForBackend(StringRef path) {
  std::string result;
  result.reserve(path.size() + 1);
  for (char c : path)
    result.push_back(isIdentChar(c) ? c : '_');
  if (result.empty() || isDigit(result.front()))
    result.insert(result.begin(), '_');
  return result;
}

// Keep the promoted partition-result namespace on the same canonical field
// path contract as module ports.  These names are consumed by both emitters
// and by ProbeRegistry, so accepting a looser grammar here would make the
// post-partition probe mapping backend-dependent.
static bool isValidFieldSegment(StringRef segment) {
  if (segment.empty() || !isIdentStart(segment.front()))
    return false;
  std::size_t index = 1;
  while (index < segment.size() && isIdentChar(segment[index]))
    ++index;
  while (index < segment.size()) {
    if (segment[index++] != '[')
      return false;
    std::size_t digitsBegin = index;
    while (index < segment.size() && isDigit(segment[index]))
      ++index;
    if (digitsBegin == index || index >= segment.size() ||
        segment[index++] != ']')
      return false;
  }
  return true;
}

static bool isValidFieldPath(StringRef path) {
  if (path.empty() || path.contains(':'))
    return false;
  SmallVector<StringRef, 8> segments;
  path.split(segments, '.', /*MaxSplit=*/-1, /*KeepEmpty=*/true);
  return llvm::all_of(segments, isValidFieldSegment);
}

// ProbeRegistry::addVec publishes only scalar leaves by appending one decimal
// `[index]` component per VectorType dimension.  Model that leaf set
// symbolically: enumerating a wide vector here would make a verifier intended
// for ultra-large designs proportional to the total lane count.
struct CanonicalProbePattern {
  std::string base;
  llvm::SmallVector<int64_t, 4> dimensions;
  uint64_t storageIdentity = 0;
  std::string owner;
};

struct SanitizedStorageOwner {
  std::string path;
  std::string owner;
  uint64_t storageIdentity = 0;
};

static FailureOr<llvm::SmallVector<uint64_t, 4>>
parseGeneratedIndexSuffix(StringRef suffix) {
  llvm::SmallVector<uint64_t, 4> indices;
  while (!suffix.empty()) {
    if (!suffix.consume_front("["))
      return failure();
    std::size_t close = suffix.find(']');
    if (close == StringRef::npos)
      return failure();
    StringRef digits = suffix.take_front(close);
    // Generated indices use std::to_string, so leading zeroes are not aliases
    // of the corresponding runtime leaf spelling.
    if (digits.empty() || (digits.size() > 1 && digits.front() == '0'))
      return failure();
    uint64_t index = 0;
    if (digits.getAsInteger(10, index))
      return failure();
    indices.push_back(index);
    suffix = suffix.drop_front(close + 1);
  }
  return indices;
}

static bool hasNonEmptyDimensions(ArrayRef<int64_t> dimensions) {
  return llvm::all_of(dimensions,
                      [](int64_t dimension) { return dimension > 0; });
}

// Return one concrete canonical leaf shared by the two symbolic patterns, or
// std::nullopt when their runtime leaf sets are disjoint.
static std::optional<std::string>
intersectCanonicalProbePatterns(const CanonicalProbePattern &lhs,
                                const CanonicalProbePattern &rhs) {
  auto intersectPrefix =
      [](const CanonicalProbePattern &prefix,
         const CanonicalProbePattern &extended) -> std::optional<std::string> {
    StringRef prefixBase(prefix.base);
    StringRef extendedBase(extended.base);
    if (!extendedBase.starts_with(prefixBase))
      return std::nullopt;

    auto fixedIndices =
        parseGeneratedIndexSuffix(extendedBase.drop_front(prefixBase.size()));
    if (failed(fixedIndices) ||
        prefix.dimensions.size() !=
            fixedIndices->size() + extended.dimensions.size() ||
        !hasNonEmptyDimensions(prefix.dimensions) ||
        !hasNonEmptyDimensions(extended.dimensions))
      return std::nullopt;

    for (auto [index, value] : llvm::enumerate(*fixedIndices)) {
      if (value >= static_cast<uint64_t>(prefix.dimensions[index]))
        return std::nullopt;
    }

    // Both remaining ranges start at zero, so index zero is a witness for
    // every dimension regardless of their respective upper bounds.
    std::string witness = extended.base;
    for (std::size_t index = 0; index < extended.dimensions.size(); ++index)
      witness += "[0]";
    return witness;
  };

  if (auto witness = intersectPrefix(lhs, rhs))
    return witness;
  if (lhs.base != rhs.base)
    return intersectPrefix(rhs, lhs);
  return std::nullopt;
}

static bool hasAnyPartitionAttribute(Operation *op) {
  return op->hasAttr(kCombPartitionParentIdAttr) ||
         op->hasAttr(kCombPartitionPartIdAttr) ||
         op->hasAttr(kCombPartitionPartCountAttr) ||
         op->hasAttr(kCombPartitionPlanVersionAttr) ||
         op->hasAttr(kCombPartitionWorkAttr) ||
         op->hasAttr(kCombPartitionMaxNodesAttr);
}

static FailureOr<uint64_t> readUnsignedAttr(pyc::CombOp comb, StringRef name) {
  auto attr = comb->getAttrOfType<IntegerAttr>(name);
  if (!attr) {
    comb.emitOpError("partitioned comb requires integer attribute '")
        << name << "'";
    return failure();
  }
  if (!attr.getType().isSignlessInteger(64)) {
    comb.emitOpError("partition attribute '")
        << name << "' must be an i64 integer";
    return failure();
  }
  int64_t value = attr.getInt();
  if (value < 0) {
    comb.emitOpError("partition attribute '")
        << name << "' must be non-negative";
    return failure();
  }
  return static_cast<uint64_t>(value);
}

static uint64_t bodyWork(pyc::CombOp comb) {
  uint64_t work = 0;
  for (Operation &op : comb.getBody().front().without_terminator())
    (void)op, ++work;
  return work;
}

struct PartitionInfo {
  pyc::CombOp comb;
  uint64_t parentId;
  uint64_t partId;
  uint64_t partCount;
  uint64_t work;
  uint64_t maxNodes;
};

struct CheckCombPartitionsPass
    : public PassWrapper<CheckCombPartitionsPass, OperationPass<func::FuncOp>> {
  MLIR_DEFINE_EXPLICIT_INTERNAL_INLINE_TYPE_ID(CheckCombPartitionsPass)

  StringRef getArgument() const override { return "pyc-check-comb-partitions"; }
  StringRef getDescription() const override {
    return "Verify sibling pyc.comb runtime partition plans";
  }

  void runOnOperation() override {
    func::FuncOp function = getOperation();
    // Declaration-only callees carry no entry block and own no emitted probe
    // storage.  Their combinational behavior is validated through the
    // hardened dependency-summary contract; attempting to fetch block
    // arguments here would dereference a non-existent entry block.
    if (function.isDeclaration())
      return;

    llvm::DenseMap<uint64_t, llvm::SmallVector<PartitionInfo>> groups;
    bool anyFailure = false;
    uint64_t observedParts = 0;
    uint64_t observedWork = 0;
    bool hasObservedMaxNodes = false;
    uint64_t observedMaxNodes = 0;
    llvm::SmallVector<CanonicalProbePattern, 16> canonicalProbePatterns;
    llvm::StringMap<SanitizedStorageOwner> sanitizedStorageOwners;
    llvm::DenseMap<Value, uint64_t> valueStorageIdentities;
    uint64_t nextStorageIdentity = 1;

    auto storageIdentityForValue = [&](Value value) {
      auto [it, inserted] = valueStorageIdentities.try_emplace(value, 0);
      if (inserted)
        it->second = nextStorageIdentity++;
      return it->second;
    };
    auto freshStorageIdentity = [&]() { return nextStorageIdentity++; };

    auto registerCanonicalProbePath = [&](StringRef path, Type type,
                                          StringRef owner,
                                          Operation *diagnosticOp,
                                          uint64_t storageIdentity) {
      if (path.empty())
        return true;
      if (!isValidFieldPath(path)) {
        diagnosticOp->emitError("invalid canonical partition probe path '")
            << path << "' from " << owner;
        anyFailure = true;
        return false;
      }

      CanonicalProbePattern pattern;
      pattern.base = path.str();
      if (auto vectorType = dyn_cast_if_present<VectorType>(type))
        llvm::append_range(pattern.dimensions, vectorType.getShape());
      pattern.storageIdentity = storageIdentity;
      pattern.owner = owner.str();

      for (const CanonicalProbePattern &existing : canonicalProbePatterns) {
        auto witness = intersectCanonicalProbePatterns(existing, pattern);
        if (!witness || existing.storageIdentity == storageIdentity)
          continue;
        diagnosticOp->emitError("duplicate canonical partition probe path '")
            << *witness << "' from " << existing.owner << " and " << owner;
        anyFailure = true;
        return false;
      }

      canonicalProbePatterns.push_back(std::move(pattern));
      return true;
    };

    // Canonical runtime paths and backend storage identifiers are distinct
    // namespaces for vectors: addVec expands canonical leaves, while C++ and
    // Verilog keep one container member named from the unsuffixed base.
    auto registerSanitizedStorageId = [&](StringRef path, StringRef owner,
                                          Operation *diagnosticOp,
                                          uint64_t storageIdentity) {
      if (path.empty())
        return;
      std::string sanitized = sanitizeIdForBackend(path);
      auto [sanitizedIt, sanitizedInserted] =
          sanitizedStorageOwners.try_emplace(
              sanitized,
              SanitizedStorageOwner{path.str(), owner.str(), storageIdentity});
      if (sanitizedInserted ||
          sanitizedIt->second.storageIdentity == storageIdentity)
        return;
      diagnosticOp->emitError(
          "partition probe path collision after backend sanitization '")
          << sanitized << "' from '" << sanitizedIt->second.path << "' and '"
          << path << "'";
      anyFailure = true;
    };

    auto registerNamedStorage = [&](StringRef path, Type type, StringRef owner,
                                    Operation *diagnosticOp,
                                    uint64_t storageIdentity) {
      if (!registerCanonicalProbePath(path, type, owner, diagnosticOp,
                                      storageIdentity))
        return;
      registerSanitizedStorageId(path, owner, diagnosticOp, storageIdentity);
    };

    // The function-level marker is the transaction header for a unified
    // partition plan.  Do not accept torn metadata: without all three fields,
    // consumers cannot distinguish a complete zero-part plan from an
    // interrupted/hand-authored plan.
    const bool hasFunctionPlan =
        function->hasAttr(kCombPartitionFunctionPlanAttr);
    const bool hasFunctionParts =
        function->hasAttr(kCombPartitionFunctionPartsAttr);
    const bool hasFunctionWork =
        function->hasAttr(kCombPartitionFunctionWorkAttr);
    const unsigned functionMetadataCount =
        static_cast<unsigned>(hasFunctionPlan) +
        static_cast<unsigned>(hasFunctionParts) +
        static_cast<unsigned>(hasFunctionWork);
    if (functionMetadataCount != 0 && functionMetadataCount != 3) {
      function.emitOpError("function partition attributes '")
          << kCombPartitionFunctionPlanAttr << "', '"
          << kCombPartitionFunctionPartsAttr << "', and '"
          << kCombPartitionFunctionWorkAttr << "' must be present together";
      signalPassFailure();
      return;
    }

    const bool hasFunctionMetadata = functionMetadataCount == 3;
    uint64_t declaredFunctionParts = 0;
    uint64_t declaredFunctionWork = 0;
    if (hasFunctionMetadata) {
      auto plan =
          function->getAttrOfType<StringAttr>(kCombPartitionFunctionPlanAttr);
      if (!plan || plan.getValue() != kCombPartitionPlanVersion) {
        function.emitOpError("function partition attribute '")
            << kCombPartitionFunctionPlanAttr << "' must equal '"
            << kCombPartitionPlanVersion << "'";
        signalPassFailure();
        return;
      }

      auto parts =
          function->getAttrOfType<IntegerAttr>(kCombPartitionFunctionPartsAttr);
      auto work =
          function->getAttrOfType<IntegerAttr>(kCombPartitionFunctionWorkAttr);
      if (!parts || !parts.getType().isSignlessInteger(64) ||
          parts.getInt() < 0) {
        function.emitOpError("function partition attribute '")
            << kCombPartitionFunctionPartsAttr
            << "' must be a non-negative i64 integer";
        signalPassFailure();
        return;
      }
      if (!work || !work.getType().isSignlessInteger(64) || work.getInt() < 0) {
        function.emitOpError("function partition attribute '")
            << kCombPartitionFunctionWorkAttr
            << "' must be a non-negative i64 integer";
        signalPassFailure();
        return;
      }
      declaredFunctionParts = static_cast<uint64_t>(parts.getInt());
      declaredFunctionWork = static_cast<uint64_t>(work.getInt());
    }

    // Probe path validity is a code-generation contract, not a partition-plan
    // transaction field.  Run it for both fully stamped static plans and the
    // supported `--comb-partition=none` path.
    if (auto names = function->getAttrOfType<ArrayAttr>("arg_names")) {
      for (auto [index, attribute] : llvm::enumerate(names)) {
        auto name = dyn_cast<StringAttr>(attribute);
        if (!name || index >= function.getNumArguments())
          continue;
        Value argument = function.getArgument(index);
        registerNamedStorage(name.getValue(), argument.getType(),
                             ("input port #" + std::to_string(index)),
                             function.getOperation(),
                             storageIdentityForValue(argument));
      }
    }
    if (auto names = function->getAttrOfType<ArrayAttr>("result_names")) {
      func::ReturnOp returnOp;
      if (!function.getBody().empty())
        returnOp = dyn_cast_or_null<func::ReturnOp>(
            function.getBody().front().getTerminator());
      for (auto [index, attribute] : llvm::enumerate(names)) {
        auto name = dyn_cast<StringAttr>(attribute);
        if (!name || index >= function.getNumResults())
          continue;
        // A named output and a named SSA value returned through that exact
        // port are public aliases of one semantic signal.  Give them one
        // owner so only unrelated namespace collisions are rejected.
        uint64_t storageIdentity =
            returnOp && index < returnOp.getNumOperands()
                ? storageIdentityForValue(returnOp.getOperand(index))
                : freshStorageIdentity();
        registerNamedStorage(name.getValue(), function.getResultTypes()[index],
                             ("output port #" + std::to_string(index)),
                             function.getOperation(), storageIdentity);
      }
    }

    // Only direct children can own C++ member storage.  Named operations cloned
    // inside a comb are local and are represented by the published comb result.
    for (Block &block : function.getBody()) {
      for (Operation &op : block) {
        if (auto comb = dyn_cast<pyc::CombOp>(op)) {
          ArrayAttr resultNames;
          if (Attribute rawResultNames = comb->getAttr(kCombResultNamesAttr)) {
            resultNames = dyn_cast<ArrayAttr>(rawResultNames);
            if (!resultNames) {
              comb.emitOpError("'")
                  << kCombResultNamesAttr << "' must be an array of strings";
              anyFailure = true;
              continue;
            }
          }
          if (resultNames && resultNames.size() != comb.getNumResults()) {
            comb.emitOpError("'")
                << kCombResultNamesAttr << "' length must match result arity";
            anyFailure = true;
            continue;
          }

          llvm::SmallVector<StringAttr> resultNameAttrs;
          resultNameAttrs.reserve(comb.getNumResults());
          bool validResultNames = true;
          for (unsigned index = 0; index < comb.getNumResults(); ++index) {
            StringAttr name;
            if (resultNames) {
              name = dyn_cast<StringAttr>(resultNames[index]);
              if (!name) {
                comb.emitOpError("'") << kCombResultNamesAttr << "' entry "
                                      << index << " must be a string";
                anyFailure = true;
                validResultNames = false;
                break;
              }
            }
            resultNameAttrs.push_back(name);
          }
          if (!validResultNames)
            continue;

          StringAttr wrapperName =
              comb.getNumResults() == 1
                  ? comb->getAttrOfType<StringAttr>("pyc.name")
                  : StringAttr();
          for (auto [index, result] : llvm::enumerate(comb.getResults())) {
            uint64_t storageIdentity = storageIdentityForValue(result);
            StringAttr resultName = resultNameAttrs[index];
            bool resultNameValid = true;
            if (resultName && !resultName.getValue().empty()) {
              resultNameValid = registerCanonicalProbePath(
                  resultName.getValue(), result.getType(),
                  ("partition result #" + std::to_string(index)),
                  comb.getOperation(), storageIdentity);
            }

            bool wrapperNameValid = true;
            if (index == 0 && wrapperName && !wrapperName.getValue().empty()) {
              wrapperNameValid = registerCanonicalProbePath(
                  wrapperName.getValue(), result.getType(),
                  "top-level pyc.comb wrapper", comb.getOperation(),
                  storageIdentity);
            }

            // NameTable chooses result_names before the legacy single-result
            // wrapper name.  Register exactly that one backend storage base;
            // any other wrapper spelling remains a canonical alias only.
            if (resultName && !resultName.getValue().empty()) {
              if (resultNameValid)
                registerSanitizedStorageId(
                    resultName.getValue(),
                    ("partition result #" + std::to_string(index)),
                    comb.getOperation(), storageIdentity);
            } else if (index == 0 && wrapperName &&
                       !wrapperName.getValue().empty() && wrapperNameValid) {
              registerSanitizedStorageId(wrapperName.getValue(),
                                         "top-level pyc.comb wrapper",
                                         comb.getOperation(), storageIdentity);
            }
          }
          continue;
        }

        if (auto name = op.getAttrOfType<StringAttr>("pyc.name");
            name && op.getNumResults() == 1) {
          Value result = op.getResult(0);
          registerNamedStorage(name.getValue(), result.getType(),
                               "top-level named value", &op,
                               storageIdentityForValue(result));
        }

        if (!isa<pyc::ByteMemOp, pyc::SyncMemOp, pyc::SyncMemDPOp>(op))
          continue;
        if (auto name = op.getAttrOfType<StringAttr>("name"))
          registerNamedStorage(name.getValue(), Type(), "memory", &op,
                               freshStorageIdentity());
      }
    }
    if (anyFailure) {
      signalPassFailure();
      return;
    }

    function.walk([&](pyc::CombOp comb) {
      if (anyFailure || !hasAnyPartitionAttribute(comb))
        return;

      auto parentId = readUnsignedAttr(comb, kCombPartitionParentIdAttr);
      auto partId = readUnsignedAttr(comb, kCombPartitionPartIdAttr);
      auto partCount = readUnsignedAttr(comb, kCombPartitionPartCountAttr);
      auto work = readUnsignedAttr(comb, kCombPartitionWorkAttr);
      auto maxNodes = readUnsignedAttr(comb, kCombPartitionMaxNodesAttr);
      auto plan =
          comb->getAttrOfType<StringAttr>(kCombPartitionPlanVersionAttr);
      if (failed(parentId) || failed(partId) || failed(partCount) ||
          failed(work) || failed(maxNodes)) {
        anyFailure = true;
        return;
      }
      if (!plan || plan.getValue() != kCombPartitionPlanVersion) {
        comb.emitOpError("partition attribute '")
            << kCombPartitionPlanVersionAttr << "' must equal '"
            << kCombPartitionPlanVersion << "'";
        anyFailure = true;
        return;
      }
      if (comb->getParentOp() != function.getOperation()) {
        comb.emitOpError(
            "unified sibling partition must be directly nested in its "
            "function");
        anyFailure = true;
        return;
      }
      if (!hasFunctionMetadata) {
        comb.emitOpError("partition plan '")
            << kCombPartitionPlanVersion
            << "' requires matching function partition metadata";
        anyFailure = true;
        return;
      }
      if (*partCount == 0 || *partId >= *partCount) {
        comb.emitOpError(
            "partition part_id must be less than non-zero part_count");
        anyFailure = true;
        return;
      }
      if (*maxNodes == 0) {
        comb.emitOpError("partition max_nodes must be positive");
        anyFailure = true;
        return;
      }
      if (!hasObservedMaxNodes) {
        observedMaxNodes = *maxNodes;
        hasObservedMaxNodes = true;
      } else if (*maxNodes != observedMaxNodes) {
        comb.emitOpError(
            "all partitions in a unified function must use the same "
            "max_nodes value");
        anyFailure = true;
        return;
      }
      if (*work == 0) {
        comb.emitOpError("partition work must be positive (empty runtime units "
                         "are illegal)");
        anyFailure = true;
        return;
      }
      uint64_t actualWork = bodyWork(comb);
      if (*work != actualWork) {
        comb.emitOpError("partition work metadata mismatch: expected ")
            << actualWork << ", got " << *work;
        anyFailure = true;
        return;
      }
      if (*work > *maxNodes) {
        comb.emitOpError("partition work exceeds max_nodes: ")
            << *work << " > " << *maxNodes;
        anyFailure = true;
        return;
      }
      for (Operation &nested : comb.getBody().front().without_terminator()) {
        if (!isMemoizableCombOperation(&nested)) {
          nested.emitError(
              "partition body operation is outside the memoization contract");
          anyFailure = true;
          return;
        }
        for (Type type : nested.getOperandTypes()) {
          if (isMemoizableCombType(type))
            continue;
          nested.emitError("partition operand cannot be compared exactly: ")
              << type;
          anyFailure = true;
          return;
        }
        for (Type type : nested.getResultTypes()) {
          if (isMemoizableCombType(type))
            continue;
          nested.emitError("partition result cannot be compared exactly: ")
              << type;
          anyFailure = true;
          return;
        }
      }
      // A partition boundary must contain direct live-ins only. Completeness is
      // enforced by CombOp's IsolatedFromAbove verifier; reject redundant
      // operands here so transitive-closure inputs cannot silently inflate
      // snapshot work or spuriously activate a runtime partition.
      Block &body = comb.getBody().front();
      ArrayAttr resultNames;
      if (Attribute rawResultNames = comb->getAttr(kCombResultNamesAttr)) {
        resultNames = dyn_cast<ArrayAttr>(rawResultNames);
        if (!resultNames) {
          comb.emitOpError("'")
              << kCombResultNamesAttr << "' must be an array of strings";
          anyFailure = true;
          return;
        }
      }
      if (resultNames && resultNames.size() != comb.getNumResults()) {
        comb.emitOpError("'")
            << kCombResultNamesAttr << "' length must match result arity";
        anyFailure = true;
        return;
      }
      if (resultNames) {
        for (auto [index, attribute] : llvm::enumerate(resultNames)) {
          if (!isa<StringAttr>(attribute)) {
            comb.emitOpError("'") << kCombResultNamesAttr << "' entry " << index
                                  << " must be a string";
            anyFailure = true;
            return;
          }
        }
      }
      for (auto [index, argument] : llvm::enumerate(body.getArguments())) {
        if (!argument.use_empty())
          continue;
        comb.emitOpError("partition has redundant live-in operand ") << index;
        anyFailure = true;
        return;
      }
      for (auto [index, result] : llvm::enumerate(comb.getResults())) {
        bool isNamed = resultNames &&
                       !cast<StringAttr>(resultNames[index]).getValue().empty();
        if (!result.use_empty() || isNamed)
          continue;
        comb.emitOpError("partition has redundant live-out result ") << index;
        anyFailure = true;
        return;
      }
      ++observedParts;
      if (std::numeric_limits<uint64_t>::max() - observedWork < *work) {
        comb.emitOpError("function partition work metadata overflows u64");
        anyFailure = true;
        return;
      }
      observedWork += *work;
      groups[*parentId].push_back(PartitionInfo{comb, *parentId, *partId,
                                                *partCount, *work, *maxNodes});
    });
    if (anyFailure) {
      signalPassFailure();
      return;
    }

    if (hasFunctionMetadata) {
      for (Block &block : function.getBody()) {
        for (Operation &op : block) {
          if (auto comb = dyn_cast<pyc::CombOp>(op)) {
            if (!hasAnyPartitionAttribute(comb)) {
              comb.emitOpError(
                  "unified function plan contains an unpartitioned pyc.comb");
              anyFailure = true;
            }
            continue;
          }
          if (!isMemoizableCombOperation(&op))
            continue;
          op.emitError("unified partition plan left a memoizable operation "
                       "outside pyc.comb");
          anyFailure = true;
        }
      }
      if (declaredFunctionParts != observedParts) {
        function.emitOpError("function partition part-count metadata mismatch: "
                             "expected ")
            << observedParts << ", got " << declaredFunctionParts;
        anyFailure = true;
      }
      if (declaredFunctionWork != observedWork) {
        function.emitOpError("function partition work metadata mismatch: "
                             "expected ")
            << observedWork << ", got " << declaredFunctionWork;
        anyFailure = true;
      }
    }

    if (anyFailure) {
      signalPassFailure();
      return;
    }

    llvm::SmallVector<uint64_t> parentIds;
    parentIds.reserve(groups.size());
    for (const auto &entry : groups)
      parentIds.push_back(entry.first);
    llvm::sort(parentIds);
    for (auto [expected, actual] : llvm::enumerate(parentIds)) {
      if (actual == expected)
        continue;
      groups[actual].front().comb.emitOpError(
          "partition parent_id values must be dense and ordered from zero; "
          "expected ")
          << expected << ", got " << actual;
      signalPassFailure();
      return;
    }

    for (auto &entry : groups) {
      auto &parts = entry.second;
      if (parts.empty())
        continue;
      uint64_t count = parts.front().partCount;
      uint64_t maxNodes = parts.front().maxNodes;
      if (parts.size() != count) {
        parts.front().comb.emitOpError("partition parent_id ")
            << entry.first << " declares " << count << " parts but has "
            << parts.size();
        anyFailure = true;
        continue;
      }

      llvm::SmallVector<pyc::CombOp> byId(count);
      for (PartitionInfo &part : parts) {
        if (part.partCount != count || part.maxNodes != maxNodes) {
          part.comb.emitOpError("partition siblings disagree on plan metadata");
          anyFailure = true;
          continue;
        }
        if (byId[part.partId]) {
          part.comb.emitOpError("duplicate partition part_id ") << part.partId;
          anyFailure = true;
          continue;
        }
        byId[part.partId] = part.comb;
      }
      if (anyFailure)
        continue;

      Block *parentBlock = byId.front()->getBlock();
      for (uint64_t i = 0; i < count; ++i) {
        pyc::CombOp comb = byId[i];
        if (!comb || comb->getBlock() != parentBlock) {
          parts.front().comb.emitOpError(
              "all sibling partitions must be present in one block");
          anyFailure = true;
          break;
        }
        if (i != 0 && byId[i - 1]->getNextNode() != comb.getOperation()) {
          comb.emitOpError(
              "sibling partitions must be adjacent and ordered by part_id");
          anyFailure = true;
          break;
        }

        // SSA dominance already rejects backward uses.  Check the explicit
        // partition contract as well so scheduler metadata cannot disagree
        // with the dependence graph it is meant to encode.
        for (Value input : comb.getInputs()) {
          auto producer = input.getDefiningOp<pyc::CombOp>();
          if (!producer)
            continue;
          auto producerParent =
              producer->getAttrOfType<IntegerAttr>(kCombPartitionParentIdAttr);
          auto producerPart =
              producer->getAttrOfType<IntegerAttr>(kCombPartitionPartIdAttr);
          if (!producerParent || !producerPart) {
            comb.emitOpError(
                "partition dependency originates from an unstamped pyc.comb");
            anyFailure = true;
            break;
          }
          uint64_t producerParentId =
              static_cast<uint64_t>(producerParent.getInt());
          if (producerParentId > entry.first) {
            comb.emitOpError(
                "cross-parent partition dependency must advance parent_id");
            anyFailure = true;
            break;
          }
          if (producerParentId < entry.first)
            continue;
          if (producerPart.getInt() >= static_cast<int64_t>(i)) {
            comb.emitOpError(
                "partition dependency must point strictly forward");
            anyFailure = true;
            break;
          }
        }
        if (anyFailure)
          break;
      }
    }

    if (anyFailure)
      signalPassFailure();
  }
};

} // namespace

std::unique_ptr<mlir::Pass> createCheckCombPartitionsPass() {
  return std::make_unique<CheckCombPartitionsPass>();
}

static PassRegistration<CheckCombPartitionsPass> pass;

} // namespace pyc
