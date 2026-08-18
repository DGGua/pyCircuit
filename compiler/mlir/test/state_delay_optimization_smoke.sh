#!/usr/bin/env bash
# Gate for Stage 0 analysis and Stage 1 structural state optimization.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
PYC_OPT="${PYC_OPT:-${ROOT}/.pycircuit_out/toolchain/build-delay-line/bin/pyc-opt}"
PYCC="${PYCC:-${ROOT}/.pycircuit_out/toolchain/build-delay-line/bin/pycc}"
INPUT="${ROOT}/compiler/mlir/test/state_delay_optimization.mlir"
DEFAULT_INPUT="${ROOT}/compiler/mlir/test/state_delay_default_structural.mlir"

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

TMP_DIR="$(mktemp -d /tmp/pyc-state-delay-opt.XXXXXX)"
trap 'rm -rf "${TMP_DIR}"' EXIT

"${PYC_OPT}" "${INPUT}" \
  --pass-pipeline='builtin.module(func.func(pyc-analyze-state-optimization))' \
  -o "${TMP_DIR}/analyze.mlir"
"${FILECHECK_BIN}" "${INPUT}" --check-prefix=ANALYZE \
  --input-file="${TMP_DIR}/analyze.mlir"

"${PYC_OPT}" "${INPUT}" \
  --pass-pipeline='builtin.module(func.func(pyc-combine-delay-chains))' \
  -o "${TMP_DIR}/generated.mlir"
"${FILECHECK_BIN}" "${INPUT}" --check-prefix=GENERATED \
  --input-file="${TMP_DIR}/generated.mlir"

"${PYC_OPT}" "${INPUT}" \
  --pass-pipeline='builtin.module(func.func(pyc-combine-delay-chains{mode=structural}))' \
  -o "${TMP_DIR}/structural.mlir"
"${FILECHECK_BIN}" "${INPUT}" --check-prefix=STRUCTURAL \
  --input-file="${TMP_DIR}/structural.mlir"

set +e
"${PYC_OPT}" "${INPUT}" \
  --pass-pipeline='builtin.module(func.func(pyc-combine-delay-chains{mode=invalid}))' \
  -o "${TMP_DIR}/invalid.mlir" >"${TMP_DIR}/invalid.stdout" \
  2>"${TMP_DIR}/invalid.stderr"
INVALID_RC=$?
set -e
if [[ ${INVALID_RC} -eq 0 ]]; then
  echo "fail: invalid delay-chain mode was accepted" >&2
  exit 1
fi
if ! grep -q "invalid delay-chain mode 'invalid' (expected generated|structural)" \
  "${TMP_DIR}/invalid.stderr"; then
  echo "fail: invalid delay-chain mode diagnostic is missing" >&2
  cat "${TMP_DIR}/invalid.stderr" >&2
  exit 1
fi

"${PYCC}" "${DEFAULT_INPUT}" --emit=cpp \
  -o "${TMP_DIR}/default.hpp" 2>"${TMP_DIR}/default.stderr"
"${PYCC}" "${DEFAULT_INPUT}" --emit=cpp --state-delay-opt=structural \
  -o "${TMP_DIR}/structural.hpp" 2>"${TMP_DIR}/structural.stderr"
"${PYCC}" "${DEFAULT_INPUT}" --emit=cpp --state-delay-opt=generated \
  -o "${TMP_DIR}/generated.hpp" 2>"${TMP_DIR}/generated.stderr"
"${PYCC}" "${DEFAULT_INPUT}" --emit=cpp --combine-delay-chains=true \
  -o "${TMP_DIR}/legacy-true.hpp" 2>"${TMP_DIR}/legacy-true.stderr"

python3 - "${TMP_DIR}/default.hpp.stats.json" \
  "${TMP_DIR}/structural.hpp.stats.json" \
  "${TMP_DIR}/generated.hpp.stats.json" \
  "${TMP_DIR}/legacy-true.hpp.stats.json" <<'PY'
import json
import sys
from pathlib import Path

default, structural, generated, legacy_true = (
    json.loads(Path(path).read_text(encoding="utf-8")) for path in sys.argv[1:]
)
keys = ("reg_count", "reg_bits", "state_opt_regs_merged",
        "state_opt_reg_bits_removed")
default_view = {key: default.get(key) for key in keys}
structural_view = {key: structural.get(key) for key in keys}
generated_view = {key: generated.get(key) for key in keys}
legacy_true_view = {key: legacy_true.get(key) for key in keys}
if default_view != structural_view:
    raise AssertionError(
        f"default policy is not structural: {default_view} != {structural_view}"
    )
expected_structural = {
    "reg_count": 1,
    "reg_bits": 8,
    "state_opt_regs_merged": 1,
    "state_opt_reg_bits_removed": 8,
}
if default_view != expected_structural:
    raise AssertionError(
        f"unexpected default structural stats: {default_view}"
    )
expected_generated = {
    "reg_count": 2,
    "reg_bits": 16,
    "state_opt_regs_merged": 0,
    "state_opt_reg_bits_removed": 0,
}
if generated_view != expected_generated:
    raise AssertionError(
        f"generated fallback changed behavior: {generated_view}"
    )
if legacy_true_view != generated_view:
    raise AssertionError(
        f"legacy true is not generated: {legacy_true_view} != {generated_view}"
    )
PY

echo "state_delay_optimization_smoke: PASS"
