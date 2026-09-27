#!/usr/bin/env bash
set -euo pipefail
repo_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)"
PYCC="${PYCC:-${repo_root}/.pycircuit_out/toolchain/build/bin/pycc}"
gate_dir="$(mktemp -d)"
trap 'rm -rf -- "${gate_dir}"' EXIT

run_verilog_oracle() {
  local expected_marker="$1"
  local status=0
  # Some VVP builds return zero after $fatal. Require both clean output and
  # the oracle's final success marker, in addition to the process status.
  vvp "${gate_dir}/sim.vvp" >"${gate_dir}/verilog.log" 2>&1 || status=$?
  cat "${gate_dir}/verilog.log"
  if [[ "${status}" -ne 0 ]] ||
      rg -q '(^|[[:space:]])(FATAL|ERROR):' "${gate_dir}/verilog.log" ||
      ! rg -Fxq -- "${expected_marker}" "${gate_dir}/verilog.log"; then
    echo 'Verilog oracle failed' >&2
    return 1
  fi
}

python3 "${repo_root}/tests/newcircuit/generate_scalar_demand_cases.py" "${gate_dir}"
fixtures=(single_not shared_dag identical_prefix fullwidth_reader kept_reader)
for case_id in "${!fixtures[@]}"; do
  fixture="${fixtures[case_id]}"
  "${PYCC}" "${gate_dir}/${fixture}.mlir" --emit=cpp \
    --dump-pass-ir="${gate_dir}/${fixture}_ir" --dump-pass-ir-filter=comb-canonicalize \
    --dump-pass-ir-phase=after -o "${gate_dir}/scalar_demand.cpp"
  python3 - "${gate_dir}/${fixture}_ir" "${case_id}" <<'PY'
from pathlib import Path
import re
import sys
source = next(Path(sys.argv[1]).glob('*.mlir')).read_text()
case_id = int(sys.argv[2])
wide = re.findall(r'pyc\.(?:add|sub|mul|not|and|or|xor)[^\n]*\bi32\b', source)
if case_id < 3:
    assert not wide, f'low-bit demand stopped before its scalar producer: {wide}'
    if case_id == 0:
        assert re.search(r'pyc\.add[^\n]*\bi4\b', source)
    if case_id == 1:
        assert re.search(r'pyc\.add[^\n]*\bi16\b', source)
    if case_id == 2:
        assert len(re.findall(r'pyc\.and[^\n]*\bi8\b', source)) == 1
else:
    assert re.search(r'pyc\.add[^\n]*\bi32\b', source), 'full-width root was narrowed'
    assert re.search(r'pyc\.not[^\n]*\bi32\b', source), 'full-width root was narrowed'
PY
  "${CXX:-c++}" -std=c++17 -O2 -I "${repo_root}/runtime" -I "${gate_dir}" \
    -DCASE_ID="${case_id}" "${repo_root}/tests/newcircuit/scalar_demand_harness.cpp" \
    -o "${gate_dir}/harness"
  "${gate_dir}/harness"
  "${PYCC}" "${gate_dir}/${fixture}.mlir" --emit=verilog -o "${gate_dir}/scalar_demand.sv"
  iverilog -g2012 -I "${repo_root}/runtime/verilog" -s tb -DCASE_ID="${case_id}" -o "${gate_dir}/sim.vvp" \
    "${gate_dir}/scalar_demand.sv" "${repo_root}/tests/newcircuit/scalar_demand_tb.sv"
  run_verilog_oracle 'scalar demand Verilog oracle passed (4096 vectors)'
  echo "PASS scalar demand ${fixture}"
done
fixtures=(dynamic_left_single dynamic_left_shared dynamic_left_full)
for case_id in "${!fixtures[@]}"; do
  fixture="${fixtures[case_id]}"
  "${PYCC}" "${gate_dir}/${fixture}.mlir" --emit=cpp \
    --dump-pass-ir="${gate_dir}/${fixture}_ir" --dump-pass-ir-filter=comb-canonicalize \
    --dump-pass-ir-phase=after -o "${gate_dir}/scalar_demand.cpp"
  python3 - "${gate_dir}/${fixture}_ir" "${case_id}" <<'PY'
from pathlib import Path
import re
import sys
source = next(Path(sys.argv[1]).glob('*.mlir')).read_text()
width = [4, 16, 128][int(sys.argv[2])]
assert re.search(r'pyc\.shl[^\n]* : i' + str(width) + r',', source)
assert re.search(r'pyc\.lshr[^\n]* : i128,', source), 'right shift lost high source bits'
PY
  "${CXX:-c++}" -std=c++17 -O2 -I "${repo_root}/runtime" -I "${gate_dir}" \
    -DCASE_ID="${case_id}" "${repo_root}/tests/newcircuit/dynamic_left_demand_harness.cpp" \
    -o "${gate_dir}/harness"
  "${gate_dir}/harness"
  "${PYCC}" "${gate_dir}/${fixture}.mlir" --emit=verilog -o "${gate_dir}/scalar_demand.sv"
  iverilog -g2012 -I "${repo_root}/runtime/verilog" -s tb -DCASE_ID="${case_id}" -o "${gate_dir}/sim.vvp" \
    "${gate_dir}/scalar_demand.sv" "${repo_root}/tests/newcircuit/dynamic_left_demand_tb.sv"
  run_verilog_oracle 'dynamic left demand Verilog oracle passed (4096 vectors)'
  echo "PASS scalar demand ${fixture}"
done
for amount_width in 64 128; do
  fixture="wide_amount_${amount_width}"
  for mode in default structural; do
    args=()
    [[ "${mode}" == structural ]] && args+=(--emit-structural=on)
    "${PYCC}" "${gate_dir}/${fixture}.mlir" --emit=cpp "${args[@]}" \
      --dump-pass-ir="${gate_dir}/${fixture}_${mode}_ir" --dump-pass-ir-filter=comb-canonicalize \
      --dump-pass-ir-phase=after -o "${gate_dir}/scalar_demand.cpp"
    python3 - "${gate_dir}/${fixture}_${mode}_ir" <<'PY'
from pathlib import Path
import re
import sys
source = next(Path(sys.argv[1]).glob('*.mlir')).read_text()
shifts = re.findall(r'pyc\.(?:shl|lshr|ashr)[^\n]*', source)
assert len(shifts) == 3 and all(re.search(r': i128, i32$', line) for line in shifts)
assert 'kept_left' in source, 'wide shift legalization lost observation metadata'
PY
    "${CXX:-c++}" -std=c++17 -O2 -I "${repo_root}/runtime" -I "${gate_dir}" \
      -DAMOUNT_WIDTH="${amount_width}" "${repo_root}/tests/newcircuit/wide_shift_amount_harness.cpp" \
      -o "${gate_dir}/harness"
    "${gate_dir}/harness"
  done
  "${PYCC}" "${gate_dir}/${fixture}.mlir" --emit=verilog -o "${gate_dir}/scalar_demand.sv"
  iverilog -g2012 -I "${repo_root}/runtime/verilog" -s tb -DAMOUNT_WIDTH="${amount_width}" \
    -o "${gate_dir}/sim.vvp" "${gate_dir}/scalar_demand.sv" \
    "${repo_root}/tests/newcircuit/wide_shift_amount_tb.sv"
  run_verilog_oracle 'wide shift amount Verilog oracle passed (4096 vectors, 12 X/Z vectors)'
  echo "PASS scalar demand ${fixture} (default/structural C++ and Verilog)"
done
echo 'NewCircuit scalar demand gate passed'
