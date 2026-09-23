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
PRIMITIVES_PY="${ROOT}/compiler/mlir/test/Inputs/comb_dirty_primitives.py"
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

for mode in "${modes[@]}"; do
  cpp_file="${OUT}/${mode}/comb_dirty_scheduler.cpp"
  grep -q '_pyc_change_schedule_schema = "pyc.change_schedule.v1"' \
    "${cpp_file}"
  grep -q '_pyc_change_schedule_node_count = 5u' "${cpp_file}"
  grep -q '_pyc_change_schedule_rank{{0u, 1u, 1u, 2u, 0u}}' \
    "${cpp_file}"
  grep -q '_pyc_change_schedule_slot{{0u, 1u, 2u, 3u, 4u}}' \
    "${cpp_file}"
done
if grep -q 'DirtyBitset<' "${OUT}/always/comb_dirty_scheduler.cpp" ||
    grep -q 'DirtyBitset<' "${OUT}/guarded/comb_dirty_scheduler.cpp"; then
  echo "fail: always/guarded unexpectedly emitted dirty skip state" >&2
  exit 1
fi
dirty_cpp="${OUT}/dirty/comb_dirty_scheduler.cpp"
grep -q 'std::array<unsigned, 2> _pyc_direct_fanout_.*{{1u, 2u}}' \
  "${dirty_cpp}"
test "$(grep -c '_pyc_direct_fanout_' "${dirty_cpp}")" -eq 6

python3 -m pycircuit.cli emit "${STATE_PY}" -o "${OUT}/comb_dirty_state.pyc"
state_dir="${OUT}/state"
mkdir -p "${state_dir}"
"${PYCC}" "${OUT}/comb_dirty_state.pyc" --emit=cpp \
  -o "${state_dir}/comb_dirty_state.cpp" --comb-update=dirty \
  "${common[@]}"
state_cpp="${state_dir}/comb_dirty_state.cpp"
if grep -q '_pyc_comb_1_input_0' "${state_cpp}"; then
  echo "fail: dirty comb still polls register Q instead of commit fanout" >&2
  exit 1
fi
grep -q '_pyc_direct_fanout_pyc_reg_4{{1u}}' "${state_cpp}"
grep -q '_pyc_commit_changed_pyc_reg_4_inst' "${state_cpp}"
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

python3 -m pycircuit.cli emit "${PRIMITIVES_PY}" \
  -o "${OUT}/comb_dirty_primitives.pyc"
primitive_dir="${OUT}/primitives"
mkdir -p "${primitive_dir}"
"${PYCC}" "${OUT}/comb_dirty_primitives.pyc" --emit=cpp \
  -o "${primitive_dir}/comb_dirty_primitives.cpp" --comb-update=dirty \
  "${common[@]}"
primitive_cpp="${primitive_dir}/comb_dirty_primitives.cpp"
for required in \
  'DirtyBitset<' \
  '_pyc_direct_fanout_' \
  'kInReadyChanged' \
  'kOutValidChanged' \
  'kOutDataChanged' \
  'kReadData0Changed' \
  'kReadData1Changed' \
  'commit_changes' \
  'source_checks' \
  'source_changes' \
  'dirty_enqueues' \
  'coalesced' \
  'max_ready'; do
  if ! grep -q "${required}" "${primitive_cpp}"; then
    echo "fail: primitive dirty source missing ${required}" >&2
    exit 1
  fi
done
cat >"${primitive_dir}/driver.cpp" <<'CPP'
#include "comb_dirty_primitives.cpp"

int main() {
  pyc::gen::comb_dirty_primitives dut;
  dut._pyc_sim_stats_enable = true;
  dut.comb();
  dut.tick();
  dut.commit();
  dut.comb();
  return dut._pyc_sim_stats.source_checks == 0;
}
CPP
"${CXX}" -std=c++17 -O0 \
  -I"${primitive_dir}" -I"${ROOT}/.pycircuit_out/toolchain/install/include" \
  "${primitive_dir}/driver.cpp" -o "${OUT}/run_primitives"
"${OUT}/run_primitives"

echo "ok: fused-comb dirty scheduler smoke"
