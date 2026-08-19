#!/usr/bin/env bash
# Gate for Stage 0 analysis and Stage 1 structural state optimization.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
PYC_OPT="${PYC_OPT:-${ROOT}/.pycircuit_out/toolchain/build-delay-line/bin/pyc-opt}"
PYCC="${PYCC:-${ROOT}/.pycircuit_out/toolchain/build-delay-line/bin/pycc}"
INPUT="${ROOT}/compiler/mlir/test/state_delay_optimization.mlir"
DEFAULT_INPUT="${ROOT}/compiler/mlir/test/state_delay_default_structural.mlir"
STAGE15_STAGE2_INPUT="${ROOT}/compiler/mlir/test/state_optimization_stage15_stage2.mlir"
PACK_PROBE_INPUT="${ROOT}/compiler/mlir/test/state_pack_probe.mlir"
OBSERVABILITY_INPUT="${ROOT}/compiler/mlir/test/state_observability_performance.mlir"

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
  --pass-pipeline='builtin.module(func.func(pyc-combine-delay-chains{mode=structural preserve-observability=true}))' \
  -o "${TMP_DIR}/structural.mlir"
"${FILECHECK_BIN}" "${INPUT}" --check-prefix=STRUCTURAL \
  --input-file="${TMP_DIR}/structural.mlir"

"${PYC_OPT}" "${INPUT}" \
  --pass-pipeline='builtin.module(func.func(pyc-combine-delay-chains{mode=structural}))' \
  -o "${TMP_DIR}/aggressive.mlir"
"${FILECHECK_BIN}" "${INPUT}" --check-prefix=AGGRESSIVE \
  --input-file="${TMP_DIR}/aggressive.mlir"

"${PYC_OPT}" "${OBSERVABILITY_INPUT}" \
  --pass-pipeline='builtin.module(func.func(pyc-analyze-state-optimization,pyc-strip-state-observability,pyc-eliminate-dead-state))' \
  -o "${TMP_DIR}/stripped-observability.mlir"
"${FILECHECK_BIN}" "${OBSERVABILITY_INPUT}" --check-prefix=STRIP \
  --input-file="${TMP_DIR}/stripped-observability.mlir"

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
  --pass-pipeline='builtin.module(func.func(pyc-pack-state-lanes{max-width=256 preserve-observability=true}))' \
  -o "${TMP_DIR}/packed-preserve.mlir"
"${FILECHECK_BIN}" "${STAGE15_STAGE2_INPUT}" --check-prefix=PRESERVE \
  --input-file="${TMP_DIR}/packed-preserve.mlir"

"${PYC_OPT}" "${STAGE15_STAGE2_INPUT}" \
  --pass-pipeline='builtin.module(func.func(pyc-pack-state-lanes{max-width=12}))' \
  -o "${TMP_DIR}/packed-cap12.mlir"
"${FILECHECK_BIN}" "${STAGE15_STAGE2_INPUT}" --check-prefix=CAP \
  --input-file="${TMP_DIR}/packed-cap12.mlir"

"${PYCC}" "${DEFAULT_INPUT}" --emit=cpp \
  -o "${TMP_DIR}/default.hpp" 2>"${TMP_DIR}/default.stderr"
"${PYCC}" "${DEFAULT_INPUT}" --emit=cpp --state-delay-opt=structural \
  -o "${TMP_DIR}/structural.hpp" 2>"${TMP_DIR}/structural.stderr"
"${PYCC}" "${DEFAULT_INPUT}" --emit=cpp --state-delay-opt=generated \
  -o "${TMP_DIR}/generated.hpp" 2>"${TMP_DIR}/generated.stderr"
"${PYCC}" "${DEFAULT_INPUT}" --emit=cpp --combine-delay-chains=true \
  -o "${TMP_DIR}/legacy-true.hpp" 2>"${TMP_DIR}/legacy-true.stderr"
"${PYCC}" "${DEFAULT_INPUT}" --emit=none \
  --probe-manifest="${TMP_DIR}/default-probes.json" \
  -o /dev/null 2>"${TMP_DIR}/default-probes.stderr"
"${PYCC}" "${PACK_PROBE_INPUT}" --emit=cpp \
  --probe-manifest="${TMP_DIR}/packed-probes.json" \
  -o "${TMP_DIR}/packed-probes.hpp" 2>"${TMP_DIR}/packed-probes.stderr"
if [[ $(grep -c 'addRegSlice<8, 16>' "${TMP_DIR}/packed-probes.hpp") -ne 2 ]]; then
  echo "fail: packed state outputs are missing sliced ProbeRegistry entries" >&2
  exit 1
fi
"${PYCC}" "${PACK_PROBE_INPUT}" --emit=cpp \
  --state-opt-preserve-observability=true \
  --probe-manifest="${TMP_DIR}/preserved-packed-probes.json" \
  -o "${TMP_DIR}/preserved-packed-probes.hpp" \
  2>"${TMP_DIR}/preserved-packed-probes.stderr"
if [[ $(grep -c 'addRegSlice<8, 16>' "${TMP_DIR}/preserved-packed-probes.hpp") -ne 4 ]]; then
  echo "fail: preservation mode lost packed state probe entries" >&2
  exit 1
fi
"${CXX:-c++}" -std=c++17 -O2 -I"${ROOT}/runtime" \
  -DMODEL_HEADER="\"${TMP_DIR}/preserved-packed-probes.hpp\"" \
  "${ROOT}/compiler/mlir/test/state_pack_probe_runtime.cpp" \
  -o "${TMP_DIR}/state-pack-probe-runtime"
"${TMP_DIR}/state-pack-probe-runtime"

python3 - "${TMP_DIR}/default.hpp.stats.json" \
  "${TMP_DIR}/structural.hpp.stats.json" \
  "${TMP_DIR}/generated.hpp.stats.json" \
  "${TMP_DIR}/legacy-true.hpp.stats.json" \
  "${TMP_DIR}/default-probes.json" \
  "${TMP_DIR}/packed-probes.json" \
  "${TMP_DIR}/preserved-packed-probes.json" <<'PY'
import json
import sys
from pathlib import Path

default, structural, generated, legacy_true = (
    json.loads(Path(path).read_text(encoding="utf-8")) for path in sys.argv[1:5]
)
probe_manifest = json.loads(Path(sys.argv[5]).read_text(encoding="utf-8"))
packed_probe_manifest = json.loads(Path(sys.argv[6]).read_text(encoding="utf-8"))
preserved_probe_manifest = json.loads(Path(sys.argv[7]).read_text(encoding="utf-8"))
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
if default.get("state_opt_policy") != "structural":
    raise AssertionError("default state optimization policy is not structural")
if default.get("state_opt_preserve_observability") is not False:
    raise AssertionError("default state optimization unexpectedly preserves observability")
if default.get("state_opt_pack_width") != 192:
    raise AssertionError("default state pack width is not 192")
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
for name in ("out0", "out1"):
    if aggressive_entries[name].get("kind") != "state":
        raise AssertionError(f"packed lane {name} is not classified as state")
for name in ("lane0_state", "lane1_state"):
    if name in aggressive_entries:
        raise AssertionError(f"performance mode retained explicit probe {name}")
preserved_entries = {
    probe["field_path"]: probe for probe in preserved_probe_manifest["probes"]
}
for name in ("out0", "out1", "lane0_state", "lane1_state"):
    if preserved_entries[name].get("kind") != "state":
        raise AssertionError(f"preservation mode lost state probe {name}")
PY

echo "state_delay_optimization_smoke: PASS"
