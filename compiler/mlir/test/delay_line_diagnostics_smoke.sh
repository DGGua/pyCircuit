#!/usr/bin/env bash
# Gate for delay-chain provenance attributes and pycc compile-stat output.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
PYC_OPT="${PYC_OPT:-${ROOT}/.pycircuit_out/toolchain/build-delay-line/bin/pyc-opt}"
PYCC="${PYCC:-${ROOT}/.pycircuit_out/toolchain/build-delay-line/bin/pycc}"
INPUT="${ROOT}/compiler/mlir/test/delay_line_diagnostics.mlir"
PRECOMBINED_INPUT="${ROOT}/compiler/mlir/test/delay_line_diagnostics_precombined.mlir"

if [[ ! -x "${PYC_OPT}" || ! -x "${PYCC}" ]]; then
  echo "fail: build pyc-opt and pycc before running this gate" >&2
  exit 1
fi

if [[ -n "${FILECHECK:-}" ]]; then
  FILECHECK_BIN="${FILECHECK}"
elif command -v FileCheck >/dev/null 2>&1; then
  FILECHECK_BIN="$(command -v FileCheck)"
elif [[ -x /usr/lib/llvm-11/bin/FileCheck ]]; then
  FILECHECK_BIN=/usr/lib/llvm-11/bin/FileCheck
else
  FILECHECK_BIN=/usr/lib/llvm-10/bin/FileCheck
fi

TMP_DIR="$(mktemp -d /tmp/pyc-delay-line-diagnostics.XXXXXX)"
trap 'rm -rf "${TMP_DIR}"' EXIT
OPTIMIZED="${TMP_DIR}/optimized.mlir"
PRECOMBINED_OPTIMIZED="${TMP_DIR}/precombined_optimized.mlir"
MODEL="${TMP_DIR}/model.hpp"
STDERR_LOG="${TMP_DIR}/pycc.stderr"
PROFILE_JSON="${TMP_DIR}/profile.json"

"${PYC_OPT}" "${INPUT}" \
  --pass-pipeline='builtin.module(func.func(pyc-combine-delay-chains,pyc-collect-compile-stats))' \
  -o "${OPTIMIZED}"
"${FILECHECK_BIN}" "${INPUT}" --input-file="${OPTIMIZED}"

"${PYC_OPT}" "${PRECOMBINED_INPUT}" \
  --pass-pipeline='builtin.module(func.func(pyc-combine-delay-chains,pyc-collect-compile-stats))' \
  -o "${PRECOMBINED_OPTIMIZED}"
"${FILECHECK_BIN}" "${PRECOMBINED_INPUT}" \
  --input-file="${PRECOMBINED_OPTIMIZED}"

"${PYCC}" "${INPUT}" --emit=cpp --combine-delay-chains=true \
  --profile-json="${PROFILE_JSON}" -o "${MODEL}" 2>"${STDERR_LOG}"

python3 - "${MODEL}.stats.json" "${STDERR_LOG}" "${PROFILE_JSON}" <<'PY'
import json
import sys
from pathlib import Path

stats_path = Path(sys.argv[1])
stderr_path = Path(sys.argv[2])
profile_path = Path(sys.argv[3])
stats = json.loads(stats_path.read_text(encoding="utf-8"))

expected = {
    "delay_chains_combined": 2,
    "delay_chain_regs_combined": 4,
    "delay_chain_aliases_removed": 2,
    "delay_chain_delay_lines_created": 2,
    "delay_chain_delay_lines_merged": 1,
    "delay_chain_state_reads_before": 4,
    "delay_chain_state_reads_after": 1,
    "delay_chain_state_writes_before": 4,
    "delay_chain_state_writes_after": 1,
}
for key, value in expected.items():
    actual = stats.get(key)
    if actual != value:
        raise AssertionError(f"{key}: expected {value}, got {actual!r}")

profile_stats = json.loads(profile_path.read_text(encoding="utf-8"))["compile_stats"]
for key, value in expected.items():
    actual = profile_stats.get(key)
    if actual != value:
        raise AssertionError(
            f"profile compile_stats.{key}: expected {value}, got {actual!r}"
        )

stderr = stderr_path.read_text(encoding="utf-8")
needle = (
    "delay_chain={chains:2, regs:4, aliases:2, created:2, merged:1, "
    "reads:4->1, writes:4->1}"
)
if needle not in stderr:
    raise AssertionError(f"missing stderr summary: {needle}\nactual stderr:\n{stderr}")
PY

echo "delay_line_diagnostics_smoke: PASS"
