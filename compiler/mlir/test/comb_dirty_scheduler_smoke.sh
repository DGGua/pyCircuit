#!/usr/bin/env bash
# Fused-comb dirty scheduling: inactivity, semantic publish filtering, direct
# fanout/reconvergence, first eval, post-commit, and removed-mode hard break.
set -euo pipefail
trap 'rc=$?; [[ ${rc} -ne 0 ]] && echo "::error title=comb-dirty::smoke failed (exit ${rc})"' EXIT

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

if "${PYCC}" --help 2>&1 | grep -q -- "--comb-update"; then
  echo "fail: pycc still exposes removed --comb-update" >&2
  exit 1
fi
export PYTHONPATH="${ROOT}/compiler/frontend:${PYTHONPATH:-}"
if python3 -m pycircuit.cli build --help 2>&1 |
    grep -q -- "--comb-update"; then
  echo "fail: pycircuit build still exposes removed --comb-update" >&2
  exit 1
fi

rm -rf "${OUT}"
mkdir -p "${OUT}"

common=(--logic-depth=256 --build-profile=dev-fast
        --inline-policy=off --hierarchy-policy=strict)
if "${PYCC}" "${INPUT}" --emit=none --comb-update=dirty -o /dev/null \
    >"${OUT}/removed-pycc-option.stdout" \
    2>"${OUT}/removed-pycc-option.stderr"; then
  echo "fail: pycc accepted removed --comb-update" >&2
  exit 1
fi
grep -q "Unknown command line argument '--comb-update=dirty'" \
  "${OUT}/removed-pycc-option.stderr"
if python3 -m pycircuit.cli build "${INSTANCE_PY}" \
    --comb-update=dirty --out-dir "${OUT}/removed-frontend-option" \
    >"${OUT}/removed-frontend-option.stdout" \
    2>"${OUT}/removed-frontend-option.stderr"; then
  echo "fail: pycircuit build accepted removed --comb-update" >&2
  exit 1
fi
grep -q 'unrecognized arguments: --comb-update=dirty' \
  "${OUT}/removed-frontend-option.stderr"

frontend_build="${OUT}/frontend-build"
python3 -m pycircuit.cli build \
  "${ROOT}/designs/examples/arith/tb_arith.py" \
  --out-dir "${frontend_build}" --target cpp --jobs 1 --profile=dev
python3 - "${frontend_build}/.build_cache.json" <<'PY'
import json
import sys

cache = json.load(open(sys.argv[1], encoding="utf-8"))
for key in ("build_flags", "cpp_build_flags"):
    if "comb_update" in cache.get(key, {}):
        raise SystemExit(f"fail: stale comb_update remains in {key}")
PY

# Failed compile/run logs land here; the CI G1 job dumps them as
# annotations so remote failures are debuggable without job-log access.
gate_log_dir="${ROOT}/docs/gates/logs/${PYC_GATE_RUN_ID:-local}/comb-dirty"
mkdir -p "${gate_log_dir}"
assert_grep() {
  local pat="$1" file="$2" msg="$3"
  if ! grep -qE "${pat}" "${file}"; then
    echo "fail: ${msg}" >&2
    echo "::error title=comb-dirty::${msg}" >&2
    exit 1
  fi
}

run_checked() {
  local name="$1"
  shift
  if ! "$@" >"${gate_log_dir}/${name}.log" 2>&1; then
    echo "fail: ${name} failed; log tail:" >&2
    tail -n 30 "${gate_log_dir}/${name}.log" >&2 || true
    echo "::error title=comb-dirty ${name}::failed; see gate-log annotations" >&2
    exit 1
  fi
}

main_dir="${OUT}/main"
mkdir -p "${main_dir}"
dirty_cpp="${main_dir}/comb_dirty_scheduler.cpp"
"${PYCC}" "${INPUT}" --emit=cpp -o "${dirty_cpp}" "${common[@]}"
run_checked compile_dirty "${CXX}" -std=c++17 -O0 \
  -I"${main_dir}" -I"${ROOT}/.pycircuit_out/toolchain/install/include" \
  "${DRIVER}" -o "${OUT}/run_dirty"
run_checked run_dirty "${OUT}/run_dirty"

"${PYCC}" "${INPUT}" --emit=verilog --include-primitives=false \
  "${common[@]}" -o "${OUT}/comb_dirty_scheduler.v"

grep -q '_pyc_change_schedule_schema = "pyc.change_schedule.v1"' \
  "${dirty_cpp}"
grep -q '_pyc_change_schedule_node_count = 5u' "${dirty_cpp}"
grep -q '_pyc_change_schedule_rank{{0u, 1u, 1u, 2u, 0u}}' \
  "${dirty_cpp}"
grep -q '_pyc_change_schedule_slot{{0u, 1u, 2u, 3u, 4u}}' \
  "${dirty_cpp}"
grep -q 'DirtyBitset<' "${dirty_cpp}"
grep -q 'std::array<unsigned, 2> _pyc_direct_fanout_.*{{1u, 2u}}' \
  "${dirty_cpp}"
test "$(grep -c '_pyc_direct_fanout_' "${dirty_cpp}")" -eq 6

python3 -m pycircuit.cli emit "${STATE_PY}" -o "${OUT}/comb_dirty_state.pyc"
state_dir="${OUT}/state"
mkdir -p "${state_dir}"
"${PYCC}" "${OUT}/comb_dirty_state.pyc" --emit=cpp \
  -o "${state_dir}/comb_dirty_state.cpp" "${common[@]}"
state_cpp="${state_dir}/comb_dirty_state.cpp"
# The comb must not snapshot the register Q directly (that is polling);
# the temp numbering shifts with benign pipeline changes, so match the
# patterns, not specific pyc_reg_N names.
if grep -qE '_pyc_comb_[0-9]+_input_[0-9]+ != pyc_reg_[0-9]+_inst->q' "${state_cpp}"; then
  echo "fail: dirty comb still polls register Q instead of commit fanout" >&2
  echo "::error title=comb-dirty::polls register Q" >&2
  exit 1
fi
assert_grep '_pyc_direct_fanout_pyc_reg_[0-9]+\{\{1u\}\}' "${state_cpp}" \
  "missing commit-wake fanout for the state register"
assert_grep '_pyc_commit_changed_pyc_reg_[0-9]+_inst' "${state_cpp}" \
  "missing commit-changed signal for the state register"
assert_grep 'if \(_pyc_commit_changed_pyc_reg_[0-9]+_inst\)' "${state_cpp}" \
  "commit-changed signal does not gate the comb re-evaluation"
assert_grep '_pyc_mark_comb_dirty\(_pyc_consumer\)' "${state_cpp}" \
  "missing dirty-consumer propagation"
run_checked compile_state "${CXX}" -std=c++17 -O0 \
  -I"${state_dir}" -I"${ROOT}/.pycircuit_out/toolchain/install/include" \
  "${STATE_DRIVER}" \
  -o "${OUT}/run_state"
run_checked run_state "${OUT}/run_state"

python3 -m pycircuit.cli emit "${INSTANCE_PY}" \
  -o "${OUT}/comb_dirty_instance.pyc"
instance_dir="${OUT}/instance"
mkdir -p "${instance_dir}"
"${PYCC}" "${OUT}/comb_dirty_instance.pyc" --emit=cpp \
  -o "${instance_dir}/comb_dirty_instance.cpp" "${common[@]}"
instance_cpp="${instance_dir}/comb_dirty_instance.cpp"
grep -q '_pyc_direct_fanout_' "${instance_cpp}"
grep -q '_pyc_mark_comb_dirty' "${instance_cpp}"
run_checked compile_instance "${CXX}" -std=c++17 -O0 \
  -I"${instance_dir}" -I"${ROOT}/.pycircuit_out/toolchain/install/include" \
  "${INSTANCE_DRIVER}" -o "${OUT}/run_instance"
run_checked run_instance "${OUT}/run_instance"

python3 -m pycircuit.cli emit "${PRIMITIVES_PY}" \
  -o "${OUT}/comb_dirty_primitives.pyc"
primitive_dir="${OUT}/primitives"
mkdir -p "${primitive_dir}"
"${PYCC}" "${OUT}/comb_dirty_primitives.pyc" --emit=cpp \
  -o "${primitive_dir}/comb_dirty_primitives.cpp" "${common[@]}"
primitive_cpp="${primitive_dir}/comb_dirty_primitives.cpp"
for required in \
  'DirtyBitset<' \
  '_pyc_direct_fanout_' \
  'kInReadyChanged' \
  'kOutValidChanged' \
  'kOutDataChanged' \
  'kReadData0Changed' \
  'kReadData1Changed' \
  '_pyc_commit_changed_' \
  '_pyc_mark_comb_dirty'; do
  if ! grep -q "${required}" "${primitive_cpp}"; then
    echo "fail: primitive dirty source missing ${required}" >&2
    exit 1
  fi
done
cat >"${primitive_dir}/driver.cpp" <<'CPP'
#include "comb_dirty_primitives.cpp"

int main() {
  pyc::gen::comb_dirty_primitives dut;
  dut.clk = pyc::cpp::Wire<1>(0);
  dut.rst = pyc::cpp::Wire<1>(0);
  dut.data = pyc::cpp::Wire<8>(7);
  dut.comb();
  if (dut.reg_data.word(0) != 1)
    return 1;
  dut.clk = pyc::cpp::Wire<1>(1);
  dut.comb();
  dut.tick();
  dut.commit();
  dut.comb();
  return dut.reg_data.word(0) == 8 ? 0 : 2;
}
CPP
run_checked compile_primitives "${CXX}" -std=c++17 -O0 \
  -I"${primitive_dir}" -I"${ROOT}/.pycircuit_out/toolchain/install/include" \
  "${primitive_dir}/driver.cpp" -o "${OUT}/run_primitives"
run_checked run_primitives "${OUT}/run_primitives"

if grep -R -E '_pyc_sim_stats|PYC_SIM_STATS|PYC_SIM_TIMING|dump_sim_stats' \
    "${OUT}" --include='*.cpp'; then
  echo "fail: generated C++ still contains simulator stats" >&2
  exit 1
fi

echo "ok: fused-comb dirty scheduler smoke"
