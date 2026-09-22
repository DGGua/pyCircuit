#!/usr/bin/env bash
# Compact gate for canonical CombDepGraph partitioning and memoization legality.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
PYCC="${PYCC:-${ROOT}/.pycircuit_out/toolchain/install/bin/pycc}"
PYC_OPT="${PYC_OPT:-${ROOT}/.pycircuit_out/toolchain/build/bin/pyc-opt}"
OUT_ROOT="${ROOT}/.pycircuit_out/gates"
mkdir -p "${OUT_ROOT}"
OUT="$(mktemp -d "${OUT_ROOT}/supernode-partition.XXXXXX")"

if [[ ! -x "${PYCC}" ]]; then
  echo "fail: pycc not built at ${PYCC}" >&2
  exit 1
fi

for flag in comb-partition comb-partition-max-nodes; do
  "${PYCC}" --help 2>&1 | rg -q -- "--${flag}"
done
if [[ -x "${PYC_OPT}" ]]; then
  "${PYC_OPT}" --help 2>&1 |
    rg -q 'pyc-(partition-comb|check-comb-memoizable|check-comb-partitions)'
fi

export PYTHONPATH="${ROOT}/compiler/frontend:${PYTHONPATH:-}"
python3 -m pycircuit.cli emit \
  "${ROOT}/compiler/mlir/test/Inputs/supernode_optional_update.py" \
  -o "${OUT}/input.pyc"

run_partition() {
  local dump_dir="$1"
  "${PYCC}" "${OUT}/input.pyc" --emit=none -o /dev/null \
    --comb-partition=static --comb-partition-max-nodes=3 \
    --logic-depth=256 --inline-policy=off --hierarchy-policy=strict \
    --dump-pass-ir="${dump_dir}" --dump-pass-ir-phase=after \
    --dump-pass-ir-filter='partition-comb'
}
run_partition "${OUT}/dump-a"
run_partition "${OUT}/dump-b"

python3 "${ROOT}/compiler/mlir/test/check_supernode_partition.py" \
  "${OUT}/dump-a" --max-nodes=3 --require-grouped \
  --expected-work=2,3,3,2,2,3,3,1
cmp "${OUT}"/dump-a/*_after_*partition-comb*.mlir \
    "${OUT}"/dump-b/*_after_*partition-comb*.mlir

"${PYCC}" "${ROOT}/compiler/mlir/test/Inputs/supernode_unused_livein.mlir" \
  --emit=none -o /dev/null --comb-partition=static \
  --comb-partition-max-nodes=1 --logic-depth=256 \
  --inline-policy=off --hierarchy-policy=strict

python3 "${ROOT}/compiler/mlir/test/check_comb_partition_contract.py" "${PYCC}"

expect_failure() {
  local input="$1"
  local diagnostic="$2"
  local log="$3"
  if "${PYCC}" "${input}" --emit=none -o /dev/null \
      --comb-partition=none --logic-depth=256 \
      --inline-policy=off --hierarchy-policy=strict >"${log}" 2>&1; then
    echo "fail: malformed input passed: ${input}" >&2
    exit 1
  fi
  rg -q "${diagnostic}" "${log}"
}
expect_failure \
  "${ROOT}/compiler/mlir/test/Inputs/supernode_invalid_partition.mlir" \
  'part_id must be less than non-zero part_count' "${OUT}/invalid.log"
expect_failure \
  "${ROOT}/compiler/mlir/test/Inputs/supernode_nonmemoizable.mlir" \
  'is not on the deterministic pyc.comb memoization whitelist' \
  "${OUT}/nonmemoizable.log"

echo "ok: compact supernode partition gate passed (${OUT})"
