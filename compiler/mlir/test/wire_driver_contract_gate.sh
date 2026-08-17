#!/usr/bin/env bash
# Gate the pre-elimination pyc.wire contract. pyc.wire is single-driver
# plumbing; it is not an implicit resolved Verilog net.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
PYCC="${PYCC:-${ROOT}/.pycircuit_out/toolchain/build/bin/pycc}"
INPUTS="${ROOT}/compiler/mlir/test/Inputs"
OUT="${PYC_WIRE_DRIVER_GATE_OUT:-${ROOT}/.pycircuit_out/gates/wire_driver_contract}"

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
  --emit=none
  -o /dev/null
  --comb-partition=static
  --comb-partition-max-nodes=3
  --logic-depth=64
  --inline-policy=off
  --hierarchy-policy=strict
)

expect_reject() {
  local input="$1"
  local diagnostic="$2"
  local label="$3"
  if "${PYCC}" "${input}" "${common_flags[@]}" \
      >"${OUT}/${label}.stdout" 2>"${OUT}/${label}.stderr"; then
    echo "fail: ${label} wire contract violation was accepted" >&2
    exit 1
  fi
  if ! grep -q "${diagnostic}" "${OUT}/${label}.stderr"; then
    echo "fail: ${label} produced an unexpected diagnostic" >&2
    cat "${OUT}/${label}.stderr" >&2
    exit 1
  fi
}

expect_reject "${INPUTS}/wire_driver_undriven_read.mlir" \
  "has no pyc.assign driver but is observable or read" undriven_read
expect_reject "${INPUTS}/wire_driver_undriven_named.mlir" \
  "has no pyc.assign driver but is observable or read" undriven_named
expect_reject "${INPUTS}/wire_driver_multiple.mlir" \
  "has multiple pyc.assign drivers" multiple

# Legal late single-driver wiring must survive the gate and the complete
# unified partition pipeline.
"${PYCC}" "${INPUTS}/wire_driver_late_single.mlir" "${common_flags[@]}"

# A named single-driver wire is a storage-backed observation root even when no
# SSA consumer reads it. EliminateWires must retain the driver so manifest,
# runtime registry, and Verilog all expose the same value.
named_cpp="${OUT}/named_cpp"
named_manifest="${OUT}/named_manifest.json"
"${PYCC}" "${INPUTS}/wire_driver_named_observable.mlir" \
  --emit=cpp --out-dir="${named_cpp}" --cpp-split=module \
  --probe-manifest="${named_manifest}" \
  --comb-partition=static --comb-partition-max-nodes=3 \
  --logic-depth=64 --inline-policy=off --hierarchy-policy=strict
python3 -c '
import json, sys
data = json.load(open(sys.argv[1], encoding="utf-8"))
paths = [entry.get("canonical_path") for entry in data.get("entries", [])]
if paths.count("dut:tap") != 1:
    raise SystemExit("named single-driver wire was not preserved in the manifest")
' "${named_manifest}"
c++ -std=c++17 -O0 \
  -I"${named_cpp}" -I"${ROOT}/runtime" \
  "${named_cpp}/wire_driver_named_observable.cpp" \
  "${INPUTS}/wire_driver_named_observable_driver.cpp" \
  -o "${OUT}/run_named_wire"
"${OUT}/run_named_wire"
"${PYCC}" "${INPUTS}/wire_driver_named_observable.mlir" \
  --emit=verilog --include-primitives=false \
  --comb-partition=static --comb-partition-max-nodes=3 \
  --logic-depth=64 --inline-policy=off --hierarchy-policy=strict \
  -o "${OUT}/named_wire.v"
grep -Eq '\btap\b' "${OUT}/named_wire.v"

echo "ok: wire-driver contract rejects 0/2+ drivers and preserves legal late/named single-driver plumbing"
