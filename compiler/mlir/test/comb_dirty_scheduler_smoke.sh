#!/usr/bin/env bash
# Fused-comb dirty scheduling: reference equivalence, inactivity, semantic
# publish filtering, direct fanout/reconvergence, first eval, and post-commit.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
PYCC="${PYCC:-${ROOT}/.pycircuit_out/toolchain/build/bin/pycc}"
CXX="${CXX:-c++}"
INPUT="${ROOT}/compiler/mlir/test/Inputs/comb_dirty_scheduler.mlir"
DRIVER="${ROOT}/compiler/mlir/test/Inputs/comb_dirty_scheduler_driver.cpp"
STATE_PY="${ROOT}/compiler/mlir/test/Inputs/comb_dirty_state.py"
STATE_DRIVER="${ROOT}/compiler/mlir/test/Inputs/comb_dirty_state_driver.cpp"
INSTANCE_PY="${ROOT}/compiler/mlir/test/Inputs/comb_dirty_instance.py"
INSTANCE_DRIVER="${ROOT}/compiler/mlir/test/Inputs/comb_dirty_instance_driver.cpp"
OUT="${ROOT}/.pycircuit_out/gates/comb_dirty_scheduler"

if [[ ! -x "${PYCC}" ]]; then
  echo "skip: pycc not built at ${PYCC}" >&2
  exit 0
fi

if ! "${PYCC}" --help 2>&1 | grep -q -- "--comb-update"; then
  echo "fail: pycc missing --comb-update" >&2
  exit 1
fi
export PYTHONPATH="${ROOT}/compiler/frontend:${PYTHONPATH:-}"
if ! python3 -m pycircuit.cli build --help 2>&1 |
    grep -q -- "--comb-update"; then
  echo "fail: pycircuit build missing --comb-update" >&2
  exit 1
fi

rm -rf "${OUT}"
mkdir -p "${OUT}"

common=(--logic-depth=256 --build-profile=dev-fast
        --inline-policy=off --hierarchy-policy=strict)
modes=(always guarded dirty)
mode_ids=(0 1 2)
for idx in "${!modes[@]}"; do
  mode="${modes[$idx]}"
  cpp_file="${OUT}/${mode}/comb_dirty_scheduler.cpp"
  mkdir -p "${OUT}/${mode}"
  "${PYCC}" "${INPUT}" --emit=cpp -o "${cpp_file}" \
    --comb-update="${mode}" "${common[@]}"
  "${CXX}" -std=c++17 -O0 -DPYC_EXPECT_COMB_MODE="${mode_ids[$idx]}" \
    -I"${OUT}/${mode}" -I"${ROOT}/.pycircuit_out/toolchain/install/include" \
    "${DRIVER}" \
    -o "${OUT}/run_${mode}"
  "${OUT}/run_${mode}"

  "${PYCC}" "${INPUT}" --emit=verilog --include-primitives=false \
    --comb-update="${mode}" "${common[@]}" -o "${OUT}/${mode}.v"
done

cmp "${OUT}/always.v" "${OUT}/guarded.v"
cmp "${OUT}/always.v" "${OUT}/dirty.v"

python3 -m pycircuit.cli emit "${STATE_PY}" -o "${OUT}/comb_dirty_state.pyc"
state_dir="${OUT}/state"
mkdir -p "${state_dir}"
"${PYCC}" "${OUT}/comb_dirty_state.pyc" --emit=cpp \
  -o "${state_dir}/comb_dirty_state.cpp" --comb-update=dirty \
  "${common[@]}"
"${CXX}" -std=c++17 -O0 \
  -I"${state_dir}" -I"${ROOT}/.pycircuit_out/toolchain/install/include" \
  "${STATE_DRIVER}" \
  -o "${OUT}/run_state"
"${OUT}/run_state"

python3 -m pycircuit.cli emit "${INSTANCE_PY}" \
  -o "${OUT}/comb_dirty_instance.pyc"
instance_dir="${OUT}/instance"
mkdir -p "${instance_dir}"
"${PYCC}" "${OUT}/comb_dirty_instance.pyc" --emit=cpp \
  -o "${instance_dir}/comb_dirty_instance.cpp" --comb-update=dirty \
  "${common[@]}"
"${CXX}" -std=c++17 -O0 \
  -I"${instance_dir}" -I"${ROOT}/.pycircuit_out/toolchain/install/include" \
  "${INSTANCE_DRIVER}" -o "${OUT}/run_instance"
"${OUT}/run_instance"

echo "ok: fused-comb dirty scheduler smoke"
