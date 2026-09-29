#!/usr/bin/env bash
# Gate for structural state optimization, delay taps, packing, and retiming.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
PYC_OPT="${PYC_OPT:-${ROOT}/.pycircuit_out/toolchain/build-delay-line/bin/pyc-opt}"
PYCC="${PYCC:-${ROOT}/.pycircuit_out/toolchain/build-delay-line/bin/pycc}"
INPUT="${ROOT}/compiler/mlir/test/state_delay_optimization.mlir"
DEFAULT_INPUT="${ROOT}/compiler/mlir/test/state_delay_default_structural.mlir"
STAGE15_STAGE2_INPUT="${ROOT}/compiler/mlir/test/state_optimization_stage15_stage2.mlir"
PACK_PROBE_INPUT="${ROOT}/compiler/mlir/test/state_pack_probe.mlir"
OBSERVABILITY_INPUT="${ROOT}/compiler/mlir/test/state_observability_performance.mlir"
RETIME_INPUT="${ROOT}/compiler/mlir/test/state_retime_pipeline.mlir"

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
  --pass-pipeline='builtin.module(func.func(pyc-combine-delay-chains))' \
  -o "${TMP_DIR}/generated.mlir"
"${FILECHECK_BIN}" "${INPUT}" --check-prefix=GENERATED \
  --input-file="${TMP_DIR}/generated.mlir"

"${PYC_OPT}" "${INPUT}" \
  --pass-pipeline='builtin.module(func.func(pyc-combine-delay-chains{mode=structural}))' \
  -o "${TMP_DIR}/structural.mlir"
"${FILECHECK_BIN}" "${INPUT}" --check-prefix=STRUCTURAL \
  --input-file="${TMP_DIR}/structural.mlir"

"${PYC_OPT}" "${INPUT}" \
  --pass-pipeline='builtin.module(func.func(pyc-combine-delay-chains{mode=structural}))' \
  -o "${TMP_DIR}/aggressive.mlir"
"${FILECHECK_BIN}" "${INPUT}" --check-prefix=AGGRESSIVE \
  --input-file="${TMP_DIR}/aggressive.mlir"

"${PYC_OPT}" "${OBSERVABILITY_INPUT}" \
  --pass-pipeline='builtin.module(func.func(pyc-eliminate-dead-state))' \
  -o "${TMP_DIR}/kept-observability.mlir"
"${FILECHECK_BIN}" "${OBSERVABILITY_INPUT}" --check-prefix=KEEP \
  --input-file="${TMP_DIR}/kept-observability.mlir"

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

"${PYC_OPT}" "${STAGE15_STAGE2_INPUT}" \
  --pass-pipeline='builtin.module(func.func(pyc-combine-delay-chains{mode=structural}),cse,func.func(pyc-combine-delay-chains{mode=structural accumulate-stats=true cascade-round=true}))' \
  -o "${TMP_DIR}/cascade.mlir"
"${FILECHECK_BIN}" "${STAGE15_STAGE2_INPUT}" --check-prefix=CASCADE \
  --input-file="${TMP_DIR}/cascade.mlir"

"${PYC_OPT}" "${STAGE15_STAGE2_INPUT}" \
  --pass-pipeline='builtin.module(func.func(pyc-pack-state-lanes{max-width=256}))' \
  -o "${TMP_DIR}/packed.mlir"
"${FILECHECK_BIN}" "${STAGE15_STAGE2_INPUT}" --check-prefix=PACK \
  --input-file="${TMP_DIR}/packed.mlir"

"${PYC_OPT}" "${STAGE15_STAGE2_INPUT}" \
  --pass-pipeline='builtin.module(func.func(pyc-pack-state-lanes{max-width=12}))' \
  -o "${TMP_DIR}/packed-cap12.mlir"
"${FILECHECK_BIN}" "${STAGE15_STAGE2_INPUT}" --check-prefix=CAP \
  --input-file="${TMP_DIR}/packed-cap12.mlir"

"${PYCC}" "${DEFAULT_INPUT}" --emit=cpp \
  -o "${TMP_DIR}/default.hpp" 2>"${TMP_DIR}/default.stderr"
"${PYCC}" "${DEFAULT_INPUT}" --emit=verilog \
  -o "${TMP_DIR}/default.v" 2>"${TMP_DIR}/default-verilog.stderr"
"${PYCC}" "${DEFAULT_INPUT}" --emit=none \
  --probe-manifest="${TMP_DIR}/default-probes.json" \
  -o /dev/null 2>"${TMP_DIR}/default-probes.stderr"
"${PYC_OPT}" "${ROOT}/compiler/mlir/test/observation_demand.mlir" \
  --pass-pipeline="builtin.module(pyc-apply-observation-demand{probe-plan=${ROOT}/compiler/mlir/test/observation_demand_probe_plan.json})" \
  -o "${TMP_DIR}/observation-demand.mlir"
"${FILECHECK_BIN}" "${ROOT}/compiler/mlir/test/observation_demand.mlir" \
  --check-prefix=DEMAND \
  --input-file="${TMP_DIR}/observation-demand.mlir"

"${PYCC}" "${PACK_PROBE_INPUT}" --emit=cpp \
  --observe-named=demand \
  -o "${TMP_DIR}/packed-probes-demand.hpp" 2>"${TMP_DIR}/packed-probes-demand.stderr"
if grep -q 'lane0_state\|lane1_state' "${TMP_DIR}/packed-probes-demand.hpp"; then
  echo "fail: demand mode kept undemanded packed lane names" >&2
  exit 1
fi
if [[ $(grep -c 'addRegSlice<8, 16>' "${TMP_DIR}/packed-probes-demand.hpp") -ge 4 ]]; then
  echo "fail: demand mode still registered undemanded packed lane slices" >&2
  exit 1
fi

"${PYCC}" "${PACK_PROBE_INPUT}" --emit=cpp \
  --observe-named=all \
  --probe-manifest="${TMP_DIR}/packed-probes.json" \
  -o "${TMP_DIR}/packed-probes.hpp" 2>"${TMP_DIR}/packed-probes.stderr"
if [[ $(grep -c 'addRegSlice<8, 16>' "${TMP_DIR}/packed-probes.hpp") -lt 4 ]]; then
  echo "fail: packed state outputs or named lanes are missing sliced ProbeRegistry entries" >&2
  exit 1
fi
"${CXX:-c++}" -std=c++17 -O2 -I"${ROOT}/runtime" \
  -DMODEL_HEADER="\"${TMP_DIR}/packed-probes.hpp\"" \
  "${ROOT}/compiler/mlir/test/state_pack_probe_runtime.cpp" \
  -o "${TMP_DIR}/state-pack-probe-runtime"
"${TMP_DIR}/state-pack-probe-runtime"

python3 - "${TMP_DIR}/default.hpp.stats.json" \
  "${TMP_DIR}/default.v.stats.json" \
  "${TMP_DIR}/default-probes.json" \
  "${TMP_DIR}/packed-probes.json" <<'PY'
import json
import sys
from pathlib import Path

default = json.loads(Path(sys.argv[1]).read_text(encoding="utf-8"))
default_verilog = json.loads(Path(sys.argv[2]).read_text(encoding="utf-8"))
probe_manifest = json.loads(Path(sys.argv[3]).read_text(encoding="utf-8"))
packed_probe_manifest = json.loads(Path(sys.argv[4]).read_text(encoding="utf-8"))
keys = ("reg_count", "reg_bits", "state_opt_regs_merged",
        "state_opt_reg_bits_removed")
default_view = {key: default.get(key) for key in keys}
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
if default.get("state_opt_policy") != "structural":
    raise AssertionError("default state optimization policy is not structural")
if default.get("state_retime_policy") != "pipeline":
    raise AssertionError("default retiming policy is not pipeline")
if "state_opt_preserve_observability" in default:
    raise AssertionError("removed preserve-observability flag leaked into stats")
if default.get("state_opt_pack_width") != 192:
    raise AssertionError("default state pack width is not 192")
if default.get("observe_named") != "demand":
    raise AssertionError("default C++ observe_named is not demand")
if default_verilog.get("state_opt_policy") != "off":
    raise AssertionError("Verilog default policy is not off")
if default_verilog.get("state_retime_policy") != "off":
    raise AssertionError("Verilog default retiming policy is not off")
if default_verilog.get("reg_count") != 2 or default_verilog.get("reg_bits") != 16:
    raise AssertionError(
        f"Verilog unexpectedly rewrote default state: {default_verilog}"
    )
output_probes = {
    probe["field_path"]: probe
    for probe in probe_manifest["probes"]
    if probe.get("dir") == "out"
}
for name in ("out0", "out1"):
    if output_probes[name].get("kind") != "state":
        raise AssertionError(f"packed optimization changed {name} probe semantics")
packed_outputs = {
    probe["field_path"]: probe
    for probe in packed_probe_manifest["probes"]
    if probe.get("dir") == "out"
}
aggressive_entries = {
    probe["field_path"]: probe for probe in packed_probe_manifest["probes"]
}
for name in ("out0", "out1", "lane0_state", "lane1_state"):
    if aggressive_entries[name].get("kind") != "state":
        raise AssertionError(f"packed lane {name} is not classified as state")
PY

python3 "${ROOT}/compiler/mlir/test/check_state_delay_tap_models.py" \
  --pycc "${PYCC}" --cxx "${CXX:-c++}"

"${PYC_OPT}" "${RETIME_INPUT}" \
  --pass-pipeline='builtin.module(func.func(pyc-retime-pipelines))' \
  -o "${TMP_DIR}/retimed.mlir"
"${FILECHECK_BIN}" "${RETIME_INPUT}" --check-prefix=RETIME \
  --input-file="${TMP_DIR}/retimed.mlir"

python3 "${ROOT}/compiler/mlir/test/check_state_retime_models.py" \
  --pycc "${PYCC}" --cxx "${CXX:-c++}"

echo "state_delay_optimization_smoke: PASS"
