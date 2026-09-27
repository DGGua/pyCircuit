#!/usr/bin/env bash
set -euo pipefail
repo_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)"
PYCC="${PYCC:-${repo_root}/.pycircuit_out/toolchain/build/bin/pycc}"
gate_dir="$(mktemp -d)"
trap 'rm -rf -- "${gate_dir}"' EXIT
python3 "${repo_root}/tests/newcircuit/generate_scalar_fixedpoint_cases.py" "${gate_dir}" > "${gate_dir}/cases.tsv"
while IFS=$'\t' read -r fixture model state full; do
  "${PYCC}" "${gate_dir}/${fixture}.mlir" --emit=cpp \
    --dump-pass-ir="${gate_dir}/${fixture}_ir" --dump-pass-ir-filter=comb-canonicalize \
    --dump-pass-ir-phase=after -o "${gate_dir}/scalar_fixedpoint.cpp"
  python3 - "${gate_dir}" "${fixture}" <<'PY'
import json
from pathlib import Path
import re
import sys
root, name = Path(sys.argv[1]), sys.argv[2]
case = next(c for c in json.loads((root / 'cases.json').read_text()) if c['name'] == name)
source = next((root / f'{name}_ir').glob('*.mlir')).read_text()
ops = re.findall(r'(?:pyc\.(?:add|mul|reg|mux|shli)|arith\.select)[^\n]*', source)
if not case['full'] and not case['keep']:
    assert not any(re.search(r'\bi(?:64|128)\b', op) for op in ops), (name, ops)
    assert re.search(r'pyc\.add[^\n]* -> i' + str(case['add_width']) + r'\b', source), (name, source)
    if case['state']:
        assert re.search(r'pyc\.reg[^\n]* : i8\b', source), (name, source)
    if name.startswith(('select_', 'mux_')) or name == 'comb_nested':
        assert re.search(r'pyc\.mux[^\n]* -> i8\b', source), (name, source)
    if case['comb']:
        interfaces = re.findall(r'pyc\.comb[^\n]*', source)
        assert interfaces and all(not re.search(r'\bi64\b', line) for line in interfaces), (name, interfaces)
else:
    assert re.search(r'pyc\.add[^\n]* -> i64\b', source), (name, source)
    if case['state']:
        assert re.search(r'pyc\.reg[^\n]* : i64\b', source), (name, source)
    if name.startswith('select_'):
        assert re.search(r'arith\.select[^\n]* : i64\b', source), (name, source)
    if case['keep']:
        assert 'pyc.debug_keep' in source, (name, source)
PY
  "${CXX:-c++}" -std=c++17 -O2 -I "${repo_root}/runtime" -I "${gate_dir}" \
    -DCASE_MODEL="${model}" -DSTATE_CASE="${state}" -DFULL_RESULT="${full}" \
    "${repo_root}/tests/newcircuit/scalar_fixedpoint_harness.cpp" -o "${gate_dir}/harness"
  "${gate_dir}/harness"
  "${PYCC}" "${gate_dir}/${fixture}.mlir" --emit=verilog -o "${gate_dir}/scalar_fixedpoint.sv"
  iverilog -g2012 -I "${repo_root}/runtime/verilog" -s tb \
    -DCASE_MODEL="${model}" -DSTATE_CASE="${state}" -DFULL_RESULT="${full}" \
    -o "${gate_dir}/sim.vvp" "${gate_dir}/scalar_fixedpoint.sv" \
    "${repo_root}/tests/newcircuit/scalar_fixedpoint_tb.sv"
  status=0
  vvp "${gate_dir}/sim.vvp" > "${gate_dir}/verilog.log" 2>&1 || status=$?
  cat "${gate_dir}/verilog.log"
  if [[ "${status}" -ne 0 ]] || rg -q '(^|[[:space:]])(FATAL|ERROR):' "${gate_dir}/verilog.log" ||
      ! rg -Fxq 'scalar fixedpoint Verilog oracle passed (4096 cycles)' "${gate_dir}/verilog.log"; then
    echo "FAIL scalar fixedpoint ${fixture} Verilog oracle" >&2
    exit 1
  fi
  echo "PASS scalar fixedpoint ${fixture}"
done < "${gate_dir}/cases.tsv"
echo 'NewCircuit scalar fixedpoint gate passed'
