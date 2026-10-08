#!/usr/bin/env bash
set -euo pipefail
repo_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)"
PYCC="${PYCC:-${repo_root}/.pycircuit_out/toolchain/build/bin/pycc}"
gate_dir="$(mktemp -d)"
trap 'rm -rf -- "${gate_dir}"' EXIT

for fixture in state instance memory; do
  harness=state
  [[ "$fixture" != memory ]] || harness=memory
  for mode in on off; do
    options=()
    [[ "$mode" == on ]] || options+=(--sim-group-activation=false)
    "$PYCC" "${repo_root}/tests/newcircuit/${fixture}_activity.mlir" \
      --emit=cpp "${options[@]}" -o "${gate_dir}/activity.cpp"
    if [[ "$mode" == on ]]; then
      rg -q '_pyc_group_active_flags' "${gate_dir}/activity.cpp"
      rg -q '_pyc_old_group_value_' "${gate_dir}/activity.cpp"
      if [[ "$fixture" == state ]]; then
        # State notifications must be published at commit, not tick-compute.
        python3 "${repo_root}/tests/newcircuit/check_commit_activity.py" \
          "${gate_dir}/activity.cpp"
      fi
    elif rg -q '_pyc_group_active_flags' "${gate_dir}/activity.cpp"; then
      echo 'disabled activation unexpectedly emitted activity flags' >&2
      exit 1
    fi
    "${CXX:-c++}" -std=c++17 -O2 -I "${repo_root}/runtime" -I "${gate_dir}" \
      "${repo_root}/tests/newcircuit/${harness}_activity_harness.cpp" \
      -o "${gate_dir}/harness"
    if [[ "$mode" == on ]]; then
      PYC_SIM_STATS=1 EXPECT_ACTIVITY=1 "${gate_dir}/harness"
    else
      PYC_SIM_STATS=1 "${gate_dir}/harness"
    fi
  done
done
for count in 8 260; do
  python3 "${repo_root}/tests/newcircuit/generate_activity_fanout.py" \
    "${gate_dir}/fanout.mlir" "$count"
  "$PYCC" "${gate_dir}/fanout.mlir" --emit=cpp --sim-replication=false \
    --sim-supernode-strict-bound=true --sim-supernode-max-size=1 \
    --logic-depth=512 \
    -o "${gate_dir}/activity.cpp"
  python3 - "${gate_dir}/activity.cpp" "$count" \
    "${repo_root}/tests/newcircuit/check_commit_activity.py" <<'PY'
import re
import runpy
import sys
checked = runpy.run_path(sys.argv[3])
source, commit = checked['source'], checked['commit']
if int(sys.argv[2]) == 8:
    assert '0ull - static_cast<std::uint64_t>' in commit
else:
    assert re.search(r'if \(pyc_reg_.* != _pyc_old_group_value_', commit)
    assert len(re.findall(r'_pyc_group_active_flags\[\d+\] \|=', commit)) > 3
assert re.search(r'group_cache_skips \+= [2-8]ull', source)
PY
  "${CXX:-c++}" -std=c++17 -O2 -DACTIVITY_LANES="$count" \
    -I "${repo_root}/runtime" -I "${gate_dir}" \
    "${repo_root}/tests/newcircuit/activity_fanout_harness.cpp" -o "${gate_dir}/fanout"
  PYC_SIM_STATS=1 "${gate_dir}/fanout"
done
echo 'NewCircuit state/instance/memory activity gate passed'
