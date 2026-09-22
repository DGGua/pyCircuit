#!/usr/bin/env bash
# Gate for module stage-DAG v1 analysis and constrained physical rewrite.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
PYC_OPT="${PYC_OPT:-${ROOT}/.pycircuit_out/toolchain/build/bin/pyc-opt}"
PYCC="${PYCC:-${ROOT}/.pycircuit_out/toolchain/build/bin/pycc}"
INPUTS="${ROOT}/compiler/mlir/test/Inputs"
OUT="${ROOT}/.pycircuit_out/gates/module_pipeline_smoke"

if [[ ! -x "${PYCC}" ]]; then
  echo "fail: build pycc before running this gate" >&2
  exit 1
fi

rm -rf "${OUT}"
mkdir -p "${OUT}"

analyze_to_file() {
  local input="$1"
  local output="$2"
  if [[ -x "${PYC_OPT}" ]]; then
    "${PYC_OPT}" "${input}" --pyc-module-pipeline -o "${output}"
    return
  fi
  local dump="${output}.dump"
  "${PYCC}" "${input}" --emit=none --module-pipeline=analyze \
    --dump-pass-ir="${dump}" --dump-pass-ir-phase=after \
    --dump-pass-ir-filter=module-pipeline -o /dev/null >/dev/null
  local files=("${dump}"/*_after_*module-pipeline*.mlir)
  if [[ "${#files[@]}" -ne 1 ]]; then
    echo "fail: expected one module-pipeline dump, got ${#files[@]}" >&2
    exit 1
  fi
  cp "${files[0]}" "${output}"
}

analyze_to_file "${INPUTS}/module_pipeline_false_scc.mlir" \
  "${OUT}/false_scc.1.mlir"
analyze_to_file "${INPUTS}/module_pipeline_false_scc.mlir" \
  "${OUT}/false_scc.2.mlir"
cmp "${OUT}/false_scc.1.mlir" "${OUT}/false_scc.2.mlir"
grep -q 'false_scc_count = 1' "${OUT}/false_scc.1.mlir"
grep -q 'mode = "analysis"' "${OUT}/false_scc.1.mlir"
grep -q 'rewritten = false' "${OUT}/false_scc.1.mlir"
grep -q 'a.out0' "${OUT}/false_scc.1.mlir"
grep -q 'b.in1' "${OUT}/false_scc.1.mlir"
if grep -q '__pyc_stage_' "${OUT}/false_scc.1.mlir"; then
  echo "fail: analysis mode claimed a physical stage rewrite" >&2
  exit 1
fi

if [[ -x "${PYC_OPT}" ]]; then
  true_cycle_cmd=("${PYC_OPT}" "${INPUTS}/module_pipeline_true_cycle.mlir"
                  --pyc-module-pipeline -o /dev/null)
else
  true_cycle_cmd=("${PYCC}" "${INPUTS}/module_pipeline_true_cycle.mlir"
                  --emit=none --module-pipeline=analyze -o /dev/null)
fi
if "${true_cycle_cmd[@]}" >"${OUT}/true_cycle.stdout" \
    2>"${OUT}/true_cycle.stderr"; then
  echo "fail: true cross-instance cycle was accepted" >&2
  exit 1
fi
grep -q 'PYC4001 cross-instance combinational cycle' \
  "${OUT}/true_cycle.stderr"
grep -q 'a.in0' "${OUT}/true_cycle.stderr"
grep -q 'b.out0' "${OUT}/true_cycle.stderr"
if "${PYCC}" "${INPUTS}/module_pipeline_true_cycle.mlir" \
    --emit=none --module-pipeline=rewrite -o /dev/null \
    >"${OUT}/true_cycle_rewrite.stdout" \
    2>"${OUT}/true_cycle_rewrite.stderr"; then
  echo "fail: true cross-instance cycle was rewritten" >&2
  exit 1
fi
grep -q 'PYC4001 cross-instance combinational cycle' \
  "${OUT}/true_cycle_rewrite.stderr"

analyze_to_file "${INPUTS}/module_pipeline_state_cut.mlir" \
  "${OUT}/state_cut.mlir"
grep -q 'state_cut_count = 1' "${OUT}/state_cut.mlir"

"${PYCC}" --help 2>&1 | grep -q -- '--module-pipeline'
rewrite_to_file() {
  local input="$1"
  local output="$2"
  local dump="${output}.dump"
  "${PYCC}" "${input}" --emit=none --module-pipeline=rewrite \
    --comb-partition=none --comb-update=dirty \
    --dump-pass-ir="${dump}" --dump-pass-ir-phase=after \
    --dump-pass-ir-filter=check-logic-depth -o /dev/null >/dev/null
  local files=("${dump}"/*_after_*check-logic-depth*.mlir)
  if [[ "${#files[@]}" -ne 1 ]]; then
    echo "fail: expected one rewrite dump, got ${#files[@]}" >&2
    exit 1
  fi
  cp "${files[0]}" "${output}"
}

rewrite_to_file "${INPUTS}/module_pipeline_false_scc.mlir" \
  "${OUT}/rewrite.1.mlir"
rewrite_to_file "${OUT}/rewrite.1.mlir" "${OUT}/rewrite.2.mlir"
grep -q 'mode = "rewrite"' "${OUT}/rewrite.1.mlir"
grep -q 'rewritten = true' "${OUT}/rewrite.1.mlir"
grep -q 'generated_stage_count = 2' "${OUT}/rewrite.1.mlir"
grep -q 'minimum_stage_count = 2' "${OUT}/rewrite.1.mlir"
test "$(grep -c 'func.func @.*__pyc_stage_0' "${OUT}/rewrite.1.mlir")" -eq 2
test "$(grep -c 'func.func @.*__pyc_stage_0' "${OUT}/rewrite.2.mlir")" -eq 2
if grep -q '__pyc_stage_1' "${OUT}/rewrite.1.mlir"; then
  echo "fail: rewrite generated a non-minimal extra stage" >&2
  exit 1
fi
if grep -q '__pyc_stage_1' "${OUT}/rewrite.2.mlir"; then
  echo "fail: idempotent rewrite generated another stage" >&2
  exit 1
fi
grep -q 'pyc.pipeline.origin = "A"' "${OUT}/rewrite.1.mlir"
grep -q 'pyc.pipeline.stage = 0' "${OUT}/rewrite.1.mlir"
grep -q 'pyc.pipeline.logical_path = "a"' "${OUT}/rewrite.1.mlir"
grep -q 'state_cut_count = 0' "${OUT}/rewrite.1.mlir"
"${PYCC}" "${INPUTS}/module_pipeline_false_scc.mlir" \
  --emit=none --module-pipeline=rewrite \
  --comb-partition=static --comb-partition-max-nodes=3 \
  --comb-update=guarded -o /dev/null >/dev/null

if "${PYCC}" "${INPUTS}/module_pipeline_unsupported_multicall.mlir" \
    --emit=none --module-pipeline=rewrite -o /dev/null \
    >"${OUT}/unsupported.stdout" 2>"${OUT}/unsupported.stderr"; then
  echo "fail: unsupported multi-callsite rewrite was accepted" >&2
  exit 1
fi
grep -q 'PYC4004 module-pipeline unsupported-edge' \
  "${OUT}/unsupported.stderr"
grep -q 'exactly one pyc.instance callsite' "${OUT}/unsupported.stderr"

echo "ok: module-pipeline analysis, rewrite, idempotence, and safety gates"
