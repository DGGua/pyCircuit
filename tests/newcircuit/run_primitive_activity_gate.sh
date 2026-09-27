#!/usr/bin/env bash
set -euo pipefail
repo_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)"
PYCC="${PYCC:-${repo_root}/.pycircuit_out/toolchain/build/bin/pycc}"
gate_dir="$(mktemp -d)"
trap 'rm -rf -- "${gate_dir}"' EXIT
python3 "${repo_root}/tests/newcircuit/generate_primitive_activity_cases.py" "${gate_dir}"
for fixture in fifo async_fifo sync_mem_dp cdc; do
  for mode in on off; do
    args=()
    [[ "${mode}" == off ]] && args+=(--sim-group-activation=false)
    "${PYCC}" "${gate_dir}/${fixture}.mlir" --emit=cpp "${args[@]}" -o "${gate_dir}/activity.cpp"
    if [[ "${mode}" == on ]]; then
      rg -q '_pyc_group_active_flags' "${gate_dir}/activity.cpp"
      rg -q '_pyc_old_group_value_' "${gate_dir}/activity.cpp"
      if [[ "${fixture}" != fifo ]]; then
        python3 - "${gate_dir}/activity.cpp" <<'PY'
from pathlib import Path
import sys
source = Path(sys.argv[1]).read_text()
commit = source.split('  void tick_commit() {', 1)[1].split('  void comb()', 1)[0]
assert '_pyc_old_group_value_' in commit, 'primitive output commit lacks activity publication'
PY
      fi
    elif rg -q '_pyc_group_active_flags' "${gate_dir}/activity.cpp"; then
      echo 'disabled primitive activity emitted flags' >&2
      exit 1
    fi
    "${CXX:-c++}" -std=c++17 -O2 -I "${repo_root}/runtime" -I "${gate_dir}" \
      "${repo_root}/tests/newcircuit/${fixture}_activity_harness.cpp" -o "${gate_dir}/harness"
    if [[ "${mode}" == on ]]; then
      PYC_SIM_STATS=1 EXPECT_ACTIVITY=1 "${gate_dir}/harness" > "${gate_dir}/${fixture}_${mode}.out"
    else
      PYC_SIM_STATS=1 "${gate_dir}/harness" > "${gate_dir}/${fixture}_${mode}.out"
    fi
  done
  cmp "${gate_dir}/${fixture}_on.out" "${gate_dir}/${fixture}_off.out"
  cat "${gate_dir}/${fixture}_on.out"
done
echo 'NewCircuit primitive activity gate passed'
