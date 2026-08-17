#!/usr/bin/env bash
# Directed gate for canonical per-result dependency transfers.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
PYCC="${PYCC:-${ROOT}/.pycircuit_out/toolchain/build/bin/pycc}"
INPUTS="${ROOT}/compiler/mlir/test/Inputs"
OUT="${PYC_COMB_DEP_GRAPH_GATE_OUT:-${ROOT}/.pycircuit_out/gates/comb_dep_graph_contract}"

if [[ ! -x "${PYCC}" ]]; then
  echo "skip: pycc not built at ${PYCC}" >&2
  exit 0
fi

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

common_flags=(
  --emit=none -o /dev/null
  --comb-update=dirty
  --comb-partition=static
  --comb-partition-max-nodes=4
  --build-profile=dev-fast
  --inline-policy=off
  --hierarchy-policy=strict
)

# Registered but unsupported result-producing operations must not silently
# acquire the old conservative all-to-all transfer.
if "${PYCC}" "${INPUTS}/comb_dep_unknown_result.mlir" \
    "${common_flags[@]}" --logic-depth=64 \
    >"${OUT}/unknown.stdout" 2>"${OUT}/unknown.stderr"; then
  echo "fail: unsupported resultful op acquired an implicit graph transfer" >&2
  exit 1
fi
grep -q "no registered canonical per-result combinational dependency transfer" \
  "${OUT}/unknown.stderr"

# The fail-closed rule must not reject supported structural @function helpers
# or the explicitly supported arith.select left after helper inlining.
"${PYCC}" "${INPUTS}/comb_dep_structural_function.mlir" \
  "${common_flags[@]}" --logic-depth=1

# BroadcastDim is structural wiring: it adds zero levels. A following real
# logic op adds exactly one, proving that a zero limit is still enforced.
"${PYCC}" "${INPUTS}/comb_dep_vbroadcast_dim_zero.mlir" \
  "${common_flags[@]}" --logic-depth=0 \
  >"${OUT}/broadcast_zero.stdout" 2>"${OUT}/broadcast_zero.stderr"
grep -q "max_depth=0/0" "${OUT}/broadcast_zero.stderr"

if "${PYCC}" "${INPUTS}/comb_dep_vbroadcast_dim_one.mlir" \
    "${common_flags[@]}" --logic-depth=0 \
    >"${OUT}/broadcast_one.stdout" 2>"${OUT}/broadcast_one.stderr"; then
  echo "fail: logic op after structural broadcast was accepted at depth 0" >&2
  exit 1
fi
grep -q "logic depth exceeds limit: depth=1 limit=0" \
  "${OUT}/broadcast_one.stderr"
"${PYCC}" "${INPUTS}/comb_dep_vbroadcast_dim_one.mlir" \
  "${common_flags[@]}" --logic-depth=1 \
  >"${OUT}/broadcast_one_ok.stdout" 2>"${OUT}/broadcast_one_ok.stderr"
grep -q "max_depth=1/1" "${OUT}/broadcast_one_ok.stderr"

echo "ok: canonical transfers fail closed and structural depth costs are exact"
