#!/usr/bin/env bash
# Gate for the GSIM-style SuperNode optional-update path:
#   * MLIR-visible static SuperNode partition metadata + verifier pipeline
#   * always / guarded / dirty C++ optional-update modes
#   * producer semantic-change filtering and fanout activity propagation
#   * scalar, fanout/reconvergence, vector, wide, and zero-input partitions
#   * C++ / Verilog semantic equivalence on the same partitioned IR
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
PYCC="${PYCC:-${ROOT}/.pycircuit_out/toolchain/build/bin/pycc}"
INPUT_PY="${ROOT}/compiler/mlir/test/Inputs/supernode_optional_update.py"
DRIVER="${ROOT}/compiler/mlir/test/Inputs/supernode_optional_update_driver.cpp"
SV_TB="${ROOT}/compiler/mlir/test/Inputs/supernode_optional_update_tb.sv"
CHECK_PARTITION="${ROOT}/compiler/mlir/test/check_supernode_partition.py"
INVALID_PARTITION="${ROOT}/compiler/mlir/test/Inputs/supernode_invalid_partition.mlir"
NONMEMOIZABLE="${ROOT}/compiler/mlir/test/Inputs/supernode_nonmemoizable.mlir"
OUT="${PYC_SUPERNODE_GATE_OUT:-${ROOT}/.pycircuit_out/gates/supernode_optional_update}"

if [[ ! -x "${PYCC}" ]]; then
  echo "skip: pycc not built at ${PYCC}" >&2
  exit 0
fi

for flag in comb-update comb-partition comb-partition-max-nodes; do
  if ! "${PYCC}" --help 2>&1 | grep -q -- "--${flag}"; then
    echo "fail: pycc missing --${flag}" >&2
    exit 1
  fi
done

gate_root="${ROOT}/.pycircuit_out/gates"
mkdir -p "${gate_root}"
OUT=$(python3 -c '
import pathlib, sys
out = pathlib.Path(sys.argv[1]).resolve()
root = pathlib.Path(sys.argv[2]).resolve()
if out == root or root not in out.parents:
    raise SystemExit(f"unsafe gate output path (must be a child of {root}): {out}")
print(out)
' "${OUT}" "${gate_root}")
rm -rf "${OUT}"
mkdir -p "${OUT}"
export PYTHONPATH="${ROOT}/compiler/frontend:${PYTHONPATH:-}"
export PYTHONDONTWRITEBYTECODE=1
frontend_help=$(python3 -m pycircuit.cli build --help 2>&1)
for flag in comb-update comb-partition comb-partition-max-nodes; do
  if ! grep -q -- "--${flag}" <<<"${frontend_help}"; then
    echo "fail: pycircuit build CLI missing --${flag}" >&2
    exit 1
  fi
done
python3 -m pycircuit.cli emit "${INPUT_PY}" -o "${OUT}/supernode_optional_update.pyc"

common_flags=(
  --comb-partition=static
  --comb-partition-max-nodes=1
  --logic-depth=256
  --build-profile=dev-fast
  --inline-policy=off
  --hierarchy-policy=strict
)

# Capture the semantic partition pass itself.  The checker requires dense IDs,
# bounded work, plan metadata, vector/wide boundaries, and a zero-input part.
"${PYCC}" "${OUT}/supernode_optional_update.pyc" \
  --emit=none -o /dev/null \
  --comb-update=dirty "${common_flags[@]}" \
  --dump-pass-ir="${OUT}/pass_ir" \
  --dump-pass-ir-phase=after \
  --dump-pass-ir-filter='partition-comb|check-comb-partitions|check-comb-memoizable'
python3 "${CHECK_PARTITION}" "${OUT}/pass_ir"

# A second structural lane locks the GSIM-style coarsen + contiguous-DP plan on
# this fanout/reconvergence DAG.  It is intentionally separate from max=1,
# which gives the runtime test precise per-producer counters.
"${PYCC}" "${OUT}/supernode_optional_update.pyc" \
  --emit=none -o /dev/null \
  --comb-update=dirty --comb-partition=static \
  --comb-partition-max-nodes=3 \
  --logic-depth=256 --build-profile=dev-fast \
  --inline-policy=off --hierarchy-policy=strict \
  --dump-pass-ir="${OUT}/pass_ir_max3" \
  --dump-pass-ir-phase=after \
  --dump-pass-ir-filter='partition-comb'
python3 "${CHECK_PARTITION}" "${OUT}/pass_ir_max3" \
  --max-nodes=3 --require-grouped \
  --expected-work=2,3,3,2,2,3,3,1
max1_parts=$(grep -hEc 'pyc\.comb\(' "${OUT}/pass_ir"/*_after_*partition-comb*.mlir)
max3_parts=$(grep -hEc 'pyc\.comb\(' "${OUT}/pass_ir_max3"/*_after_*partition-comb*.mlir)
if [[ "${max3_parts}" -ge "${max1_parts}" ]]; then
  echo "fail: max=3 did not reduce part count (${max3_parts} vs ${max1_parts})" >&2
  exit 1
fi

# Determinism is part of the hardened scheduler metadata contract: identical
# IR/options must produce byte-identical normalized post-partition IR.
"${PYCC}" "${OUT}/supernode_optional_update.pyc" \
  --emit=none -o /dev/null \
  --comb-update=dirty --comb-partition=static \
  --comb-partition-max-nodes=3 \
  --logic-depth=256 --build-profile=dev-fast \
  --inline-policy=off --hierarchy-policy=strict \
  --dump-pass-ir="${OUT}/pass_ir_max3_repeat" \
  --dump-pass-ir-phase=after \
  --dump-pass-ir-filter='partition-comb'
max3_first=$(find "${OUT}/pass_ir_max3" -maxdepth 1 -type f \
  -name '*_after_*partition-comb*.mlir' -print -quit)
max3_repeat=$(find "${OUT}/pass_ir_max3_repeat" -maxdepth 1 -type f \
  -name '*_after_*partition-comb*.mlir' -print -quit)
if [[ -z "${max3_first}" || -z "${max3_repeat}" ]] || \
   ! cmp -s "${max3_first}" "${max3_repeat}"; then
  echo "fail: repeated static partition did not produce byte-identical IR" >&2
  exit 1
fi

# Gate-first negative case: malformed scheduler metadata must be rejected by
# the normal pycc pipeline before either backend can consume it.  Partitioning
# is disabled so the rewrite cannot repair/replace the deliberately bad attrs.
set +e
invalid_pycc_output=$("${PYCC}" "${INVALID_PARTITION}" \
  --emit=none -o /dev/null \
  --comb-update=dirty --comb-partition=none \
  --logic-depth=256 --build-profile=dev-fast \
  --inline-policy=off --hierarchy-policy=strict 2>&1)
invalid_pycc_rc=$?
set -e
if [[ "${invalid_pycc_rc}" -eq 0 ]]; then
  echo "fail: malformed partition metadata passed the normal pycc pipeline" >&2
  exit 1
fi
if ! grep -q 'part_id must be less than non-zero part_count' \
  <<<"${invalid_pycc_output}"; then
  echo "fail: pycc did not fail in pyc-check-comb-partitions" >&2
  echo "${invalid_pycc_output}" >&2
  exit 1
fi
echo "ok: pycc rejected malformed partition metadata"

# MemoryEffectFree is not sufficient for memoization.  An otherwise valid
# pyc.comb containing an unlisted generic pure op must fail before codegen.
set +e
nonmemoizable_output=$("${PYCC}" "${NONMEMOIZABLE}" \
  --emit=none -o /dev/null \
  --comb-update=dirty --comb-partition=none \
  --logic-depth=256 --build-profile=dev-fast \
  --inline-policy=off --hierarchy-policy=strict 2>&1)
nonmemoizable_rc=$?
set -e
if [[ "${nonmemoizable_rc}" -eq 0 ]]; then
  echo "fail: non-whitelisted pure comb op passed memoization gate" >&2
  exit 1
fi
if ! grep -q 'is not on the deterministic pyc.comb memoization whitelist' \
  <<<"${nonmemoizable_output}"; then
  echo "fail: nonmemoizable comb produced an unexpected diagnostic" >&2
  echo "${nonmemoizable_output}" >&2
  exit 1
fi
echo "ok: pycc rejected non-whitelisted pure comb op"

# pyc-opt gives a narrower direct-pass check when LLVM exports the optional
# MLIRRegisterAllPasses target.  Keep it as an additional sub-gate, but pycc
# above is mandatory on every supported toolchain.
PYC_OPT="${PYC_OPT:-${PYCC%/pycc}/pyc-opt}"
if [[ -x "${PYC_OPT}" ]]; then
  set +e
  invalid_output=$("${PYC_OPT}" "${INVALID_PARTITION}" \
    --pyc-check-comb-partitions 2>&1)
  invalid_rc=$?
  set -e
  if [[ "${invalid_rc}" -eq 0 ]]; then
    echo "fail: malformed partition metadata passed pyc-check-comb-partitions" >&2
    exit 1
  fi
  if ! grep -q 'part_id must be less than non-zero part_count' \
    <<<"${invalid_output}"; then
    echo "fail: malformed partition produced an unexpected diagnostic" >&2
    echo "${invalid_output}" >&2
    exit 1
  fi
else
  echo "skip: pyc-opt not built; negative partition-verifier sub-gate skipped" >&2
fi

# Generate and run all three execution policies.  The one shared driver checks
# both values and the stable comb counters exposed by generated SimObjects.
modes=(always guarded dirty)
mode_ids=(0 1 2)
for idx in "${!modes[@]}"; do
  mode="${modes[$idx]}"
  mode_id="${mode_ids[$idx]}"
  cpp_dir="${OUT}/cpp_${mode}"
  "${PYCC}" "${OUT}/supernode_optional_update.pyc" \
    --emit=cpp --out-dir="${cpp_dir}" --cpp-split=module \
    --comb-update="${mode}" "${common_flags[@]}"

  for counter in \
    comb_guard_checks comb_eval_calls comb_cache_skips \
    comb_output_store_attempts comb_output_semantic_changes comb_fanout_enqueues; do
    if ! grep -q "${counter}" "${cpp_dir}/supernode_optional_update.hpp"; then
      echo "fail: generated ${mode} header is missing counter ${counter}" >&2
      exit 1
    fi
  done

  c++ -std=c++17 -O0 \
    -DPYC_EXPECT_COMB_MODE="${mode_id}" \
    -I"${cpp_dir}" -I"${ROOT}/runtime" \
    "${cpp_dir}/supernode_optional_update.cpp" "${DRIVER}" \
    -o "${OUT}/run_${mode}"
  "${OUT}/run_${mode}"
done

# Execute the coarsened max=3 plan too.  Its first scalar partition has two
# live-outs, so the masked-input phase verifies that a multi-output producer
# can run without publishing either unchanged output or waking its consumer.
grouped_dir="${OUT}/cpp_dirty_grouped"
"${PYCC}" "${OUT}/supernode_optional_update.pyc" \
  --emit=cpp --out-dir="${grouped_dir}" --cpp-split=module \
  --comb-update=dirty --comb-partition=static \
  --comb-partition-max-nodes=3 \
  --logic-depth=256 --build-profile=dev-fast \
  --inline-policy=off --hierarchy-policy=strict
c++ -std=c++17 -O0 \
  -DPYC_EXPECT_COMB_MODE=2 -DPYC_EXPECT_GROUPED=1 \
  -I"${grouped_dir}" -I"${ROOT}/runtime" \
  "${grouped_dir}/supernode_optional_update.cpp" "${DRIVER}" \
  -o "${OUT}/run_dirty_grouped"
"${OUT}/run_dirty_grouped"

# `none` is a useful reference/debug switch: fusion remains a single comb and
# no partition metadata is hardened into that generated module.
none_dir="${OUT}/cpp_none"
"${PYCC}" "${OUT}/supernode_optional_update.pyc" \
  --emit=cpp --out-dir="${none_dir}" --cpp-split=module \
  --comb-update=guarded --comb-partition=none \
  --logic-depth=256 --build-profile=dev-fast \
  --inline-policy=off --hierarchy-policy=strict
none_methods=$(grep -Ec '^  void eval_comb_[0-9]+\(\);' \
  "${none_dir}/supernode_optional_update.hpp")
if [[ "${none_methods}" -ne 1 ]]; then
  echo "fail: --comb-partition=none emitted ${none_methods} comb methods (expected 1)" >&2
  exit 1
fi

# Verilog consumes the same partitioned MLIR but ignores the C++ scheduling
# policy.  Icarus verifies the exact values used by the C++ driver.
if command -v iverilog >/dev/null 2>&1 && command -v vvp >/dev/null 2>&1; then
  "${PYCC}" "${OUT}/supernode_optional_update.pyc" \
    --emit=verilog --include-primitives=false \
    --comb-update=dirty "${common_flags[@]}" \
    -o "${OUT}/supernode_optional_update.v"
  iverilog -g2012 -s supernode_optional_update_tb \
    -o "${OUT}/run_verilog" \
    "${OUT}/supernode_optional_update.v" "${SV_TB}"
  vvp "${OUT}/run_verilog"
else
  echo "skip: iverilog/vvp unavailable; C++ modes and MLIR structure passed" >&2
fi

echo "ok: SuperNode optional-update gate passed"
