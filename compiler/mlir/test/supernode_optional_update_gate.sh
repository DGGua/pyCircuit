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
INSTANCE_BOUNDARY_PY="${ROOT}/compiler/mlir/test/Inputs/supernode_instance_boundary.py"
STATE_BOUNDARY_PY="${ROOT}/compiler/mlir/test/Inputs/supernode_state_boundary.py"
STATE_BOUNDARY_DRIVER="${ROOT}/compiler/mlir/test/Inputs/supernode_state_boundary_driver.cpp"
REG_COMMIT_DRIVER="${ROOT}/compiler/mlir/test/Inputs/pyc_reg_commit_driver.cpp"
DRIVER="${ROOT}/compiler/mlir/test/Inputs/supernode_optional_update_driver.cpp"
SV_TB="${ROOT}/compiler/mlir/test/Inputs/supernode_optional_update_tb.sv"
CHECK_PARTITION="${ROOT}/compiler/mlir/test/check_supernode_partition.py"
INVALID_PARTITION="${ROOT}/compiler/mlir/test/Inputs/supernode_invalid_partition.mlir"
NONMEMOIZABLE="${ROOT}/compiler/mlir/test/Inputs/supernode_nonmemoizable.mlir"
UNUSED_LIVEIN="${ROOT}/compiler/mlir/test/Inputs/supernode_unused_livein.mlir"
PARTITION_CONTRACT_CHECKER="${ROOT}/compiler/mlir/test/check_comb_partition_contract.py"
NAMED_PROBE_PY="${ROOT}/compiler/mlir/test/Inputs/supernode_named_probe.py"
NAMED_PROBE_DRIVER="${ROOT}/compiler/mlir/test/Inputs/supernode_named_probe_driver.cpp"
DUPLICATE_RESULT_NAMES="${ROOT}/compiler/mlir/test/Inputs/supernode_duplicate_result_names.mlir"
DUPLICATE_RESULT_NAMES_DRIVER="${ROOT}/compiler/mlir/test/Inputs/supernode_duplicate_result_names_driver.cpp"
OUT="${PYC_SUPERNODE_GATE_OUT:-${ROOT}/.pycircuit_out/gates/supernode_optional_update}"

if [[ ! -x "${PYCC}" ]]; then
  echo "skip: pycc not built at ${PYCC}" >&2
  exit 0
fi

for flag in comb-update comb-reg-update comb-partition comb-partition-max-nodes; do
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
for flag in comb-update comb-reg-update comb-partition comb-partition-max-nodes; do
  if ! grep -q -- "--${flag}" <<<"${frontend_help}"; then
    echo "fail: pycircuit build CLI missing --${flag}" >&2
    exit 1
  fi
done
c++ -std=c++17 -O0 -I"${ROOT}/runtime" \
  "${REG_COMMIT_DRIVER}" -o "${OUT}/run_reg_commit"
"${OUT}/run_reg_commit"
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

# The function marker and every sibling Comb form one hardened transaction.
# Exercise all-or-none function attrs, marker/stamp agreement, exact summary
# totals, non-empty units, and result-name schema validation in one MLIR gate.
python3 "${PARTITION_CONTRACT_CHECKER}" "${PYCC}"

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

# A legal pre-existing one-operation comb may carry an unused operand.  The
# unified materializer must rebuild even a single partition with exact direct
# live-ins; merely stamping the original comb reproduces the old verifier
# failure ("partition has redundant live-in operand").
"${PYCC}" "${UNUSED_LIVEIN}" \
  --emit=none -o /dev/null \
  --comb-update=dirty --comb-partition=static \
  --comb-partition-max-nodes=35 \
  --logic-depth=256 --build-profile=dev-fast \
  --inline-policy=off --hierarchy-policy=strict \
  --dump-pass-ir="${OUT}/pass_ir_unused_livein" \
  --dump-pass-ir-phase=after \
  --dump-pass-ir-filter='partition-comb|check-comb-partitions'
unused_dump=$(find "${OUT}/pass_ir_unused_livein" -maxdepth 1 -type f \
  -name '*_after_*partition-comb*.mlir' -print -quit)
if [[ -z "${unused_dump}" ]]; then
  echo "fail: no partition dump for unused-live-in positive regression" >&2
  exit 1
fi
unused_comb_line=$(grep -m1 'pyc\.comb(' "${unused_dump}")
if [[ "${unused_comb_line}" != *'pyc.comb(%arg0)'* ]] || \
   [[ "${unused_comb_line}" == *'%arg1'* ]]; then
  echo "fail: single partition did not rebuild exact live-ins" >&2
  echo "${unused_comb_line}" >&2
  exit 1
fi
echo "ok: single partition rebuilt exact direct live-ins"

# State/wire and hierarchy boundaries remain top-level scheduling operations,
# but pure candidates on either side must still be formed directly from the
# function-level graph.  These compact structural lanes also cover a
# zero-input constant stage at a register boundary.
python3 -m pycircuit.cli emit \
  "${STATE_BOUNDARY_PY}" \
  -o "${OUT}/state_boundary.pyc"
"${PYCC}" "${OUT}/state_boundary.pyc" \
  --emit=none -o /dev/null \
  --comb-update=dirty --comb-partition=static \
  --comb-partition-max-nodes=3 \
  --logic-depth=256 --build-profile=dev-fast \
  --inline-policy=off --hierarchy-policy=strict \
  --dump-pass-ir="${OUT}/pass_ir_state_boundary" \
  --dump-pass-ir-phase=after --dump-pass-ir-filter='partition-comb'
state_dump=$(find "${OUT}/pass_ir_state_boundary" -maxdepth 1 -type f \
  -name '*_after_*partition-comb*.mlir' -print -quit)
if [[ -z "${state_dump}" ]] || \
   ! grep -q '^    %.* = pyc\.reg ' "${state_dump}" || \
   ! grep -q '^    %.* = pyc\.wire ' "${state_dump}" || \
   ! grep -q '^    pyc\.assign ' "${state_dump}" || \
   ! grep -q 'pyc\.comb()' "${state_dump}"; then
  echo "fail: state/wire boundary was not preserved around unified combs" >&2
  [[ -n "${state_dump}" ]] && sed -n '1,180p' "${state_dump}" >&2
  exit 1
fi
if grep -q '^      %.* = pyc\.reg ' "${state_dump}"; then
  echo "fail: stateful pyc.reg was cloned inside a memoized comb" >&2
  exit 1
fi
state_parents=$(grep -o 'pyc\.partition\.parent_id = [0-9]*' \
  "${state_dump}" | sort -u | wc -l)
if [[ "${state_parents}" -lt 2 ]]; then
  echo "fail: register boundary did not split placement-safe parent plans" >&2
  exit 1
fi
echo "ok: state/wire boundary remained outside ${state_parents} parent plans"

python3 -m pycircuit.cli emit "${INSTANCE_BOUNDARY_PY}" \
  -o "${OUT}/instance_boundary.pyc"
"${PYCC}" "${OUT}/instance_boundary.pyc" \
  --emit=none -o /dev/null \
  --comb-update=dirty --comb-partition=static \
  --comb-partition-max-nodes=3 \
  --logic-depth=256 --build-profile=dev-fast \
  --inline-policy=off --hierarchy-policy=strict \
  --dump-pass-ir="${OUT}/pass_ir_instance_boundary" \
  --dump-pass-ir-phase=after --dump-pass-ir-filter='partition-comb'
instance_top_dump=$(find "${OUT}/pass_ir_instance_boundary" -maxdepth 1 \
  -type f -name '*_after_*partition-comb*.mlir' \
  -exec grep -l 'pyc\.instance ' {} \; | head -n1)
if [[ -z "${instance_top_dump}" ]] || \
   ! grep -q '^    %.* = pyc\.instance ' "${instance_top_dump}"; then
  echo "fail: hierarchy boundary disappeared during unified partitioning" >&2
  exit 1
fi
if grep -q '^      %.* = pyc\.instance ' "${instance_top_dump}"; then
  echo "fail: pyc.instance was cloned inside a memoized comb" >&2
  exit 1
fi
instance_parents=$(grep -o 'pyc\.partition\.parent_id = [0-9]*' \
  "${instance_top_dump}" | sort -u | wc -l)
if [[ "${instance_parents}" -lt 2 ]]; then
  echo "fail: instance boundary did not split placement-safe parent plans" >&2
  exit 1
fi
echo "ok: instance boundary remained outside ${instance_parents} parent plans"

# Both backends must mechanically consume the boundary-preserving IR.  The
# main fixture below supplies value-level C++/Verilog equivalence; these are
# compact syntax/compile smokes for state and hierarchical module emission.
compile_boundary_backends() {
  local label="$1"
  local input="$2"
  local top="$3"
  local cpp_dir="${OUT}/cpp_boundary_${label}"
  "${PYCC}" "${input}" \
    --emit=cpp --out-dir="${cpp_dir}" --cpp-split=module \
    --comb-update=dirty --comb-partition=static \
    --comb-partition-max-nodes=3 \
    --logic-depth=256 --build-profile=dev-fast \
    --inline-policy=off --hierarchy-policy=strict
  local source
  for source in "${cpp_dir}"/*.cpp; do
    c++ -std=c++17 -O0 -I"${cpp_dir}" -I"${ROOT}/runtime" \
      -c "${source}" -o "${source%.cpp}.o"
  done

  if command -v iverilog >/dev/null 2>&1; then
    "${PYCC}" "${input}" \
      --emit=verilog --include-primitives=true \
      --comb-update=dirty --comb-partition=static \
      --comb-partition-max-nodes=3 \
      --logic-depth=256 --build-profile=dev-fast \
      --inline-policy=off --hierarchy-policy=strict \
      -o "${OUT}/${label}_boundary.v"
    iverilog -g2012 -I"${ROOT}/runtime/verilog" -s "${top}" \
      -o "${OUT}/compile_${label}_boundary_verilog" \
      "${OUT}/${label}_boundary.v"
  fi
}
compile_boundary_backends \
  state "${OUT}/state_boundary.pyc" supernode_state_boundary
compile_boundary_backends \
  instance "${OUT}/instance_boundary.pyc" supernode_instance_boundary
echo "ok: state/instance boundary IR compiled in C++ and Verilog backends"

# Compare the current per-consumer register polling path with commit-driven
# direct-RegOp fanout. Both lanes execute the same state transition sequence.
for reg_update in poll commit; do
  state_cpp_dir="${OUT}/cpp_state_reg_${reg_update}"
  "${PYCC}" "${OUT}/state_boundary.pyc" \
    --emit=cpp --out-dir="${state_cpp_dir}" --cpp-split=module \
    --comb-update=dirty --comb-reg-update="${reg_update}" \
    --comb-partition=static --comb-partition-max-nodes=3 \
    --logic-depth=256 --build-profile=dev-fast \
    --inline-policy=off --hierarchy-policy=strict

  if [[ "${reg_update}" == "commit" ]]; then
    if grep -q '_pyc_comb_1_input_0' \
        "${state_cpp_dir}/supernode_state_boundary.hpp"; then
      echo "fail: commit mode retained the direct register input snapshot" >&2
      exit 1
    fi
    if ! grep -q 'reg_commit_checks' \
        "${state_cpp_dir}/supernode_state_boundary.hpp" || \
       ! grep -q '_pyc_comb_mark_active(1u)' \
        "${state_cpp_dir}/supernode_state_boundary.cpp"; then
      echo "fail: commit mode did not emit register change fanout" >&2
      exit 1
    fi
    reg_update_id=1
  else
    if ! grep -q '_pyc_comb_1_input_0' \
        "${state_cpp_dir}/supernode_state_boundary.hpp"; then
      echo "fail: poll mode removed the direct register input snapshot" >&2
      exit 1
    fi
    reg_update_id=0
  fi

  c++ -std=c++17 -O0 \
    -DPYC_EXPECT_REG_UPDATE="${reg_update_id}" \
    -I"${state_cpp_dir}" -I"${ROOT}/runtime" \
    "${state_cpp_dir}/supernode_state_boundary.cpp" \
    "${STATE_BOUNDARY_DRIVER}" \
    -o "${OUT}/run_state_reg_${reg_update}"
  "${OUT}/run_state_reg_${reg_update}"
done
echo "ok: poll and commit-driven register invalidation are equivalent"

# The register mode only changes dirty scheduling. Guarded+commit must retain
# complete input snapshots and must not consume register commit fanout.
state_guarded_dir="${OUT}/cpp_state_guarded_commit"
"${PYCC}" "${OUT}/state_boundary.pyc" \
  --emit=cpp --out-dir="${state_guarded_dir}" --cpp-split=module \
  --comb-update=guarded --comb-reg-update=commit \
  --comb-partition=static --comb-partition-max-nodes=3 \
  --logic-depth=256 --build-profile=dev-fast \
  --inline-policy=off --hierarchy-policy=strict
if ! grep -q '_pyc_comb_1_input_0' \
    "${state_guarded_dir}/supernode_state_boundary.hpp"; then
  echo "fail: guarded+commit removed a required register snapshot" >&2
  exit 1
fi
c++ -std=c++17 -O0 -DPYC_EXPECT_REG_UPDATE=0 \
  -I"${state_guarded_dir}" -I"${ROOT}/runtime" \
  "${state_guarded_dir}/supernode_state_boundary.cpp" \
  "${STATE_BOUNDARY_DRIVER}" \
  -o "${OUT}/run_state_guarded_commit"
"${OUT}/run_state_guarded_commit"
echo "ok: guarded+commit remains on complete snapshot semantics"

# A named value that remains semantically live at the unified-pass boundary
# must retain its identity when its defining op becomes local to a sibling
# Comb. Static partitioning promotes it to a storage-backed result used
# consistently by the manifest, generated C++ ProbeRegistry, and Verilog.
python3 -m pycircuit.cli emit "${NAMED_PROBE_PY}" \
  -o "${OUT}/named_probe.pyc"
named_probe_dir="${OUT}/cpp_named_probe"
named_probe_manifest="${OUT}/named_probe_manifest.json"
"${PYCC}" "${OUT}/named_probe.pyc" \
  --emit=cpp --out-dir="${named_probe_dir}" --cpp-split=module \
  --probe-manifest="${named_probe_manifest}" \
  --comb-update=dirty --comb-partition=static \
  --comb-partition-max-nodes=3 \
  --logic-depth=256 --build-profile=dev-fast \
  --inline-policy=off --hierarchy-policy=strict
python3 -c '
import json, sys
data = json.load(open(sys.argv[1], encoding="utf-8"))
matches = [entry for entry in data.get("entries", [])
           if entry.get("canonical_path") == "dut:tap"]
if len(matches) != 1 or matches[0].get("dir") != "internal" or matches[0].get("width_bits") != 8:
    raise SystemExit("named probe manifest did not preserve the unique internal dut:tap entry")
' "${named_probe_manifest}"
c++ -std=c++17 -O0 \
  -I"${named_probe_dir}" -I"${ROOT}/runtime" \
  "${named_probe_dir}/supernode_named_probe.cpp" "${NAMED_PROBE_DRIVER}" \
  -o "${OUT}/run_named_probe_registry"
"${OUT}/run_named_probe_registry"
"${PYCC}" "${OUT}/named_probe.pyc" \
  --emit=verilog --include-primitives=false \
  --comb-update=dirty --comb-partition=static \
  --comb-partition-max-nodes=3 \
  --logic-depth=256 --build-profile=dev-fast \
  --inline-policy=off --hierarchy-policy=strict \
  -o "${OUT}/named_probe.v"
if ! grep -Eq '\btap\b' "${OUT}/named_probe.v"; then
  echo "fail: static Verilog emission lost named internal probe tap" >&2
  exit 1
fi
echo "ok: named probe tap survived manifest, runtime registry, and Verilog"

# Two independently named wrapper results are allowed to yield the same body
# value.  Unfolding/replanning must retain both names instead of overwriting
# the first pyc.name, and a second static replan must be idempotent.
duplicate_first_dump="${OUT}/pass_ir_duplicate_names_first"
"${PYCC}" "${DUPLICATE_RESULT_NAMES}" \
  --emit=none -o /dev/null \
  --comb-update=dirty --comb-partition=static \
  --comb-partition-max-nodes=3 \
  --logic-depth=256 --build-profile=dev-fast \
  --inline-policy=off --hierarchy-policy=strict \
  --dump-pass-ir="${duplicate_first_dump}" \
  --dump-pass-ir-phase=after --dump-pass-ir-filter='partition-comb'
duplicate_first_ir=$(find "${duplicate_first_dump}" -maxdepth 1 -type f \
  -name '*_after_*partition-comb*.mlir' -print -quit)
if [[ -z "${duplicate_first_ir}" ]]; then
  echo "fail: duplicate-result-name first partition dump is missing" >&2
  exit 1
fi
duplicate_second_dir="${OUT}/cpp_duplicate_names_second"
duplicate_second_manifest="${OUT}/duplicate_names_second_manifest.json"
"${PYCC}" "${duplicate_first_ir}" \
  --emit=cpp --out-dir="${duplicate_second_dir}" --cpp-split=module \
  --probe-manifest="${duplicate_second_manifest}" \
  --comb-update=dirty --comb-partition=static \
  --comb-partition-max-nodes=3 \
  --logic-depth=256 --build-profile=dev-fast \
  --inline-policy=off --hierarchy-policy=strict \
  --dump-pass-ir="${OUT}/pass_ir_duplicate_names_second" \
  --dump-pass-ir-phase=after --dump-pass-ir-filter='partition-comb'
python3 - "${duplicate_second_manifest}" <<'PY'
import json
import pathlib
import sys

data = json.loads(pathlib.Path(sys.argv[1]).read_text(encoding="utf-8"))
paths = {entry.get("canonical_path") for entry in data.get("entries", [])}
expected = {"dut:tap_a", "dut:tap_b"}
if not expected.issubset(paths):
    raise SystemExit(f"duplicate-result names were lost: expected {expected}, got {paths}")
PY
c++ -std=c++17 -O0 \
  -I"${duplicate_second_dir}" -I"${ROOT}/runtime" \
  "${duplicate_second_dir}/supernode_duplicate_result_names.cpp" \
  "${DUPLICATE_RESULT_NAMES_DRIVER}" \
  -o "${OUT}/run_duplicate_result_names_registry"
"${OUT}/run_duplicate_result_names_registry"
duplicate_second_ir=$(find "${OUT}/pass_ir_duplicate_names_second" \
  -maxdepth 1 -type f -name '*_after_*partition-comb*.mlir' -print -quit)
for promoted_name in tap_a tap_b; do
  if [[ -z "${duplicate_second_ir}" ]] || \
     ! grep -q "${promoted_name}" "${duplicate_second_ir}"; then
    echo "fail: repeated static replan lost ${promoted_name}" >&2
    exit 1
  fi
done
first_work=$(grep -o 'pyc\.partition\.function_work = [0-9]*' \
  "${duplicate_first_ir}" | head -n1)
second_work=$(grep -o 'pyc\.partition\.function_work = [0-9]*' \
  "${duplicate_second_ir}" | head -n1)
if [[ -z "${first_work}" || "${first_work}" != "${second_work}" ]]; then
  echo "fail: repeated static replan changed duplicate-name work metadata" >&2
  echo "first=${first_work} second=${second_work}" >&2
  exit 1
fi
echo "ok: shared yielded value retained both result names across replan"

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

# The local policy must preserve the same dirty producer-change propagation
# while partitioning only inside FuseComb-created regions.
local_dir="${OUT}/cpp_dirty_local"
"${PYCC}" "${OUT}/supernode_optional_update.pyc" \
  --emit=cpp --out-dir="${local_dir}" --cpp-split=module \
  --comb-update=dirty --comb-partition=local \
  --comb-partition-max-nodes=1 \
  --logic-depth=256 --build-profile=dev-fast \
  --inline-policy=off --hierarchy-policy=strict
c++ -std=c++17 -O0 -DPYC_EXPECT_COMB_MODE=2 \
  -I"${local_dir}" -I"${ROOT}/runtime" \
  "${local_dir}/supernode_optional_update.cpp" "${DRIVER}" \
  -o "${OUT}/run_dirty_local"
"${OUT}/run_dirty_local"
echo "ok: local fused-comb partitions preserve dirty activity propagation"

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

# C++ code-size chunking is independent of semantic SuperNode partitioning.
# Force every operation into a separate eval_comb_N_part_K method while each
# MLIR partition may still contain up to three operations. Yielded candidates
# must be struct-owned so the wrapper can publish them after part methods
# return; a Local annotation here produces out-of-scope C++ identifiers.
chunked_dir="${OUT}/cpp_dirty_grouped_chunk1"
"${PYCC}" "${OUT}/supernode_optional_update.pyc" \
  --emit=cpp --out-dir="${chunked_dir}" --cpp-split=module \
  --cpp-shard-max-ast-nodes=1 \
  --comb-update=dirty --comb-partition=static \
  --comb-partition-max-nodes=3 \
  --logic-depth=256 --build-profile=dev-fast \
  --inline-policy=off --hierarchy-policy=strict
mapfile -t chunked_sources < <(find "${chunked_dir}" -maxdepth 1 \
  -type f -name '*.cpp' -print | sort)
if [[ "${#chunked_sources[@]}" -eq 0 ]]; then
  echo "fail: chunked C++ lane emitted no sources" >&2
  exit 1
fi
c++ -std=c++17 -O0 \
  -DPYC_EXPECT_COMB_MODE=2 -DPYC_EXPECT_GROUPED=1 \
  -I"${chunked_dir}" -I"${ROOT}/runtime" \
  "${chunked_sources[@]}" "${DRIVER}" \
  -o "${OUT}/run_dirty_grouped_chunk1"
"${OUT}/run_dirty_grouped_chunk1"
echo "ok: yielded partition values survive C++ part-method chunking"

# `none` is a useful legacy reference/debug switch.  The deliberate pyc.assert
# boundary in this fixture splits FuseComb into two physical runs, while the
# static lane above recovers one unified graph-derived parent plan.
none_dir="${OUT}/cpp_none"
"${PYCC}" "${OUT}/supernode_optional_update.pyc" \
  --emit=cpp --out-dir="${none_dir}" --cpp-split=module \
  --comb-update=guarded --comb-partition=none \
  --logic-depth=256 --build-profile=dev-fast \
  --inline-policy=off --hierarchy-policy=strict
none_methods=$(grep -Ec '^  void eval_comb_[0-9]+\(\);' \
  "${none_dir}/supernode_optional_update.hpp")
if [[ "${none_methods}" -ne 2 ]]; then
  echo "fail: --comb-partition=none emitted ${none_methods} comb methods (expected 2 legacy runs)" >&2
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
