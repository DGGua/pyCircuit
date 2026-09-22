#!/usr/bin/env bash
# Gate for the analysis-only module stage-DAG v1 contract.
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
grep -q 'mode = "analysis-only"' "${OUT}/false_scc.1.mlir"
grep -q 'rewritten = false' "${OUT}/false_scc.1.mlir"
grep -q 'a.out0' "${OUT}/false_scc.1.mlir"
grep -q 'b.in1' "${OUT}/false_scc.1.mlir"
if grep -q '__pyc_stage_' "${OUT}/false_scc.1.mlir"; then
  echo "fail: analysis-only v1 claimed a physical stage rewrite" >&2
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

analyze_to_file "${INPUTS}/module_pipeline_state_cut.mlir" \
  "${OUT}/state_cut.mlir"
grep -q 'state_cut_count = 1' "${OUT}/state_cut.mlir"

"${PYCC}" --help 2>&1 | grep -q -- '--module-pipeline'
if "${PYCC}" "${INPUTS}/module_pipeline_false_scc.mlir" \
    --emit=none --module-pipeline=rewrite -o /dev/null \
    >"${OUT}/rewrite.stdout" 2>"${OUT}/rewrite.stderr"; then
  echo "fail: unsupported physical rewrite was silently accepted" >&2
  exit 1
fi
grep -q 'rewrite is not implemented in v1' "${OUT}/rewrite.stderr"

echo "ok: module-pipeline analysis, cycle gate, state cuts, and wiring"
