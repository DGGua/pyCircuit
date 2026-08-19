#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
PYCC="${PYCC:-${ROOT}/.pycircuit_out/toolchain/build/bin/pycc}"
INPUT="${ROOT}/compiler/mlir/test/Inputs/comb_host_boundary.mlir"
OUT="$(mktemp -d "${TMPDIR:-/tmp}/pyc-comb-host-boundary.XXXXXX")"
trap 'rm -rf "${OUT}"' EXIT

"${PYCC}" "${INPUT}" --emit=cpp --out-dir="${OUT}/cpp" --logic-depth=256 \
  --dump-pass-ir="${OUT}/ir" \
  --dump-pass-ir-filter='comb-host-boundary' >/dev/null

AFTER="$(find "${OUT}/ir" -name '*_after_*comb-host-boundary*.mlir' -print -quit)"
if [[ -z "${AFTER}" ]]; then
  echo "fail: missing post-pass IR dump" >&2
  exit 1
fi

if grep -q 'pyc\.comb(%arg0, %arg0, %arg1)' "${AFTER}"; then
  echo "fail: duplicate/unused inputs survived" >&2
  cat "${AFTER}" >&2
  exit 1
fi
if ! grep -q 'pyc.name = "observed"' "${AFTER}"; then
  echo "fail: named observable was removed" >&2
  cat "${AFTER}" >&2
  exit 1
fi
if ! grep -Eq 'return %[^,]+, %arg0 : i8, i8' "${AFTER}"; then
  echo "fail: duplicate output did not collapse to the original input" >&2
  cat "${AFTER}" >&2
  exit 1
fi
if [[ ! -f "${OUT}/cpp/comb_host_boundary.cpp" ]]; then
  echo "fail: C++ emission did not complete" >&2
  exit 1
fi

echo "ok: comb host boundary normalized"
