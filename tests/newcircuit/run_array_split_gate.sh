#!/usr/bin/env bash
# Decisions 0121/0136/0140/0147: shared MLIR splitting preserves observation
# values and names before either backend emits code.
set -euo pipefail
repo_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)"
source "${repo_root}/flows/scripts/lib.sh"
if [[ -z "${PYCC:-}" && -x "${repo_root}/.pycircuit_out/toolchain/build/bin/pycc" ]]; then
  PYCC="${repo_root}/.pycircuit_out/toolchain/build/bin/pycc"
fi
pyc_find_pycc
if [[ ! -d "${PYC_TOOLCHAIN_ROOT}/include/cpp" ]]; then
  PYC_TOOLCHAIN_ROOT="${repo_root}/.pycircuit_out/toolchain/install"
fi
gate_dir="$(mktemp -d)"
trap 'rm -rf -- "${gate_dir}"' EXIT
designs=(array_probe_lanes array_probe_aggregate array_probe_reductions array_probe_roots array_probe_slp array_probe_memory)
for design in "${designs[@]}"; do
  "${PYCC}" "${repo_root}/tests/newcircuit/${design}.mlir" --emit=cpp \
    --dump-pass-ir="${gate_dir}/${design}_ir" \
    --dump-pass-ir-filter=comb-canonicalize -o "${gate_dir}/${design}.cpp"
done
python3 - "${gate_dir}" <<'PY'
import pathlib, re, sys
root = pathlib.Path(sys.argv[1])
lanes = sorted((root / 'array_probe_lanes_ir').glob('*after*.mlir'))
assert lanes, 'missing lane-split IR'
for path in lanes:
    text = path.read_text()
    for name in ('sum_probe', 'duplicate_probe', 'created_probe', 'broadcast_probe', 'state_probe'):
        assert re.search(r'pyc.alias[^\n]*pyc.name = "' + name + '"', text), (path, name)
    assert len(re.findall(r'= pyc.reg[^\n]*: i8', text)) == 2, path
    assert len(re.findall(r'= pyc.add[^\n]*: i8', text)) == 2, path
    assert not re.search(r'= pyc.(?:reg|add)[^\n]*vector<', text), path
    assert any('pyc.alias' in line and 'pyc.debug_keep' in line and 'pyc.name' not in line
               for line in text.splitlines()), path
aggregates = sorted((root / 'array_probe_aggregate_ir').glob('*after*.mlir'))
assert aggregates, 'missing aggregate IR'
for path in aggregates:
    text = path.read_text()
    for op, name in (('add', 'aggregate_add'), ('mux', 'aggregate_mux'), ('v_create', 'aggregate_create')):
        assert re.search(r'pyc.' + op + r'[^\n]*pyc.name = "' + name + r'"[^\n]*vector<', text), (path, name)
    assert re.search(r'pyc.alias[^\n]*pyc.debug_keep[^\n]*vector<3xi8>', text), path
reductions = sorted((root / 'array_probe_reductions_ir').glob('*after*.mlir'))
assert reductions, 'missing reduction IR'
for path in reductions:
    text = path.read_text()
    for name in ('or_probe', 'and_probe', 'sum_probe', 'row_probe', 'column_probe'):
        assert re.search(r'pyc.alias[^\n]*pyc.name = "' + name + '"', text), (path, name)
    assert 'pyc.v_broadcast_dim' not in text, path
    assert not re.search(r'pyc.v_(?:or|and|add)_reduce[^\n]*dim =', text), path
roots = sorted((root / 'array_probe_roots_ir').glob('*after*.mlir'))
assert roots, 'missing debug root IR'
for path in roots:
    text = path.read_text()
    for name in ('kept_sum', 'kept_duplicate', 'kept_identity', 'nested_keep', 'kept_wire'):
        assert re.search(r'pyc.alias[^\n]*pyc.name = "' + name + '"', text), (path, name)
    assert 'pyc.name = "kept_state"' in text, path
    for name in ('kept_flag0', 'kept_flag1'):
        assert re.search(r'pyc.reg[^\n]*pyc.name = "' + name + '"', text), (path, name)
    assert len(re.findall(r'= pyc.add ', text)) == 1, path
    for name in ('unused_name_hint', 'unused_state_hint', 'disabled_keep'):
        assert name not in text, (path, name)
slps = sorted((root / 'array_probe_slp_ir').glob('*after*.mlir'))
assert slps, 'missing SLP observation IR'
for path in slps:
    text = path.read_text()
    for op, name in (('xor', 'xor0'), ('xor', 'xor1'), ('not', 'not0'), ('not', 'not1'),
                     ('mux', 'mux0'), ('mux', 'mux1'), ('v_create', 'equal_pair')):
        assert re.search(r'pyc.' + op + r'[^\n]*pyc.name = "' + name + '"', text), (path, name)
memories = sorted((root / 'array_probe_memory_ir').glob('*after*.mlir'))
assert memories, 'missing observed memory IR'
for path in memories:
    text = path.read_text()
    assert re.search(r'pyc.sync_mem [^\n]*name = "mem16"[^\n]*: i2, i16, i2', text), path
    assert re.search(r'pyc.sync_mem_dp [^\n]*name = "mem32"[^\n]*: i2, i32, i4', text), path
PY
command -v iverilog >/dev/null || pyc_die "array splitting gate requires iverilog"
for design in "${designs[@]}"; do
  "${CXX:-c++}" -std=c++17 -I "${PYC_TOOLCHAIN_ROOT}/include" -I "${gate_dir}" \
    "${repo_root}/tests/newcircuit/${design}_harness.cpp" -o "${gate_dir}/${design}_harness"
  "${gate_dir}/${design}_harness"
  "${PYCC}" "${repo_root}/tests/newcircuit/${design}.mlir" \
    --emit=verilog -o "${gate_dir}/${design}.v"
  iverilog -g2012 -s "${design}_tb" -I "${repo_root}/runtime/verilog" \
    -o "${gate_dir}/${design}_tb" "${gate_dir}/${design}.v" \
    "${repo_root}/tests/newcircuit/${design}_tb.sv"
  "${gate_dir}/${design}_tb"
done
# Optional vector unrolling must retain aggregate storage and live lane names.
for design in array_probe_roots array_probe_lanes array_probe_slp array_probe_aggregate; do
  "${PYCC}" "${repo_root}/tests/newcircuit/${design}.mlir" \
    --unroll-vector --emit=cpp -o "${gate_dir}/${design}.cpp"
  "${CXX:-c++}" -std=c++17 -I "${PYC_TOOLCHAIN_ROOT}/include" -I "${gate_dir}" \
    "${repo_root}/tests/newcircuit/${design}_harness.cpp" -o "${gate_dir}/unrolled_harness"
  "${gate_dir}/unrolled_harness"
  "${PYCC}" "${repo_root}/tests/newcircuit/${design}.mlir" \
    --unroll-vector --emit=verilog -o "${gate_dir}/unrolled.v"
  iverilog -g2012 -s "${design}_tb" -I "${repo_root}/runtime/verilog" \
    -o "${gate_dir}/unrolled_tb" "${gate_dir}/unrolled.v" \
    "${repo_root}/tests/newcircuit/${design}_tb.sv"
  "${gate_dir}/unrolled_tb"
done
python3 - "${repo_root}" "${gate_dir}" <<'PY'
import pathlib, sys
repo, out = map(pathlib.Path, sys.argv[1:])
source = (repo / 'tests/newcircuit/array_probe_roots.mlir').read_text()
prefix = source.split('    %zero =', 1)[0]
(out / 'observed_cycle.mlir').write_text(prefix + '''    %w = pyc.wire : i8
    %sum = pyc.add %w, %a {pyc.name = "cycle_probe", pyc.debug_keep = true} : i8, i8 -> i8
    pyc.assign %w, %sum : i8
    return %b : i8
  }
}
''')
memory = (repo / 'tests/newcircuit/array_probe_memory.mlir').read_text()
(out / 'anonymous_memory.mlir').write_text(memory.replace(', name = "mem16"', ''))
PY
if "${PYCC}" "${gate_dir}/observed_cycle.mlir" --emit=cpp \
    -o "${gate_dir}/cycle.cpp" >"${gate_dir}/cycle.log" 2>&1; then
  pyc_die "observation effect incorrectly hid a hardware combinational cycle"
fi
rg -q 'combinational cycle' "${gate_dir}/cycle.log"
if "${PYCC}" "${gate_dir}/anonymous_memory.mlir" --emit=cpp \
    -o "${gate_dir}/anonymous_memory.cpp" >"${gate_dir}/anonymous_memory.log" 2>&1; then
  pyc_die "anonymous memory bypassed the mandatory observation contract"
fi
rg -q '\[PYC942\].*missing required.*name' "${gate_dir}/anonymous_memory.log"
printf '%s\n' 'NewCircuit observable array lane splitting gate: PASS'
