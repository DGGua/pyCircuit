#!/usr/bin/env bash
# Public synthetic compile-budget regression; never accepts application source.
# The 300s default bounds each complete build (up to 31 TUs), not each TU.
# It does not modify the external benchmark compiler flags or its timeout.
set -euo pipefail
repo_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)"
: "${PYCC:?set PYCC explicitly to the compiler under test}"
cxx="${CXX:-clang++}"
budget="${PYC_LARGE_COMPILE_SECONDS:-300}"
gate_dir="${PYC_LARGE_GATE_DIR:-$(mktemp -d)}"
mkdir -p "${gate_dir}"
gate_dir="$(realpath "${gate_dir}")"
if [[ -z "${PYC_LARGE_GATE_DIR:-}" ]]; then trap 'rm -rf -- "${gate_dir}"' EXIT; fi
"${cxx}" --version | head -n 1
printf 'Direct/header flags: -O3; split Release: -O3 -DNDEBUG; per-build wall budget=%ss; build parallelism=1\n' "${budget}"
python3 "${repo_root}/tests/newcircuit/generate_large_state_probe.py" "${gate_dir}" --count 1024
"${PYCC}" "${gate_dir}/large_state_probe.mlir" --emit=cpp -o "${gate_dir}/large_state_probe.cpp"
python3 "${repo_root}/tests/newcircuit/check_large_state_probe_chunks.py" "${gate_dir}/large_state_probe.cpp"
harness="${repo_root}/tests/newcircuit/large_state_probe_harness.cpp"
compile_bounded() {
  local name="$1"; shift
  if ! /usr/bin/time -f 'compile_wall_seconds=%e peak_rss_kib=%M' -o "${gate_dir}/${name}.resources" \
      timeout --kill-after=5 "${budget}" "$@" > "${gate_dir}/${name}.build.log" 2>&1; then
    cat "${gate_dir}/${name}.build.log" "${gate_dir}/${name}.resources"
    return 1
  fi
  cat "${gate_dir}/${name}.resources"
}
compile_bounded direct "${cxx}" -std=c++17 -O3 \
  -I "${repo_root}/runtime" -I "${gate_dir}" "${harness}" -o "${gate_dir}/direct-oracle"
PYC_SIM_STATS=1 "${gate_dir}/direct-oracle" > "${gate_dir}/direct.oracle"
cat "${gate_dir}/direct.oracle"

"${PYCC}" "${gate_dir}/large_state_probe.mlir" --emit=cpp --cpp-split=module --cpp-pch --cpp-shard-threshold-lines=1024 \
  --out-dir="${gate_dir}/split" --cpp-manifest="${gate_dir}/split/manifest.json"
python3 "${repo_root}/tests/newcircuit/check_large_state_probe_chunks.py" \
  "${gate_dir}/large_state_probe.cpp" --split-dir "${gate_dir}/split"
python3 - "${gate_dir}" "${repo_root}" <<'PY'
import json
from pathlib import Path
import sys
root, repo = map(Path, sys.argv[1:])
directory = root / 'split'
manifest = json.loads((directory / 'manifest.json').read_text())
pch = manifest.get('precompile_headers')
assert manifest.get('precompile_headers_mode') == 'device_hpp' and pch
assert all(Path(p).is_file() for p in pch)
sources = [(directory / source['path']).resolve() for source in manifest['sources']]
sources.append(repo / 'tests/newcircuit/large_state_probe_harness.cpp')
quote = lambda path: json.dumps(str(path))
lines = ['cmake_minimum_required(VERSION 3.20)', 'project(large_state_probe LANGUAGES CXX)',
         'set(CMAKE_CXX_STANDARD 17)', 'set(CMAKE_CXX_STANDARD_REQUIRED ON)',
         'add_executable(state_oracle ' + ' '.join(map(quote, sources)) + ')',
         'target_include_directories(state_oracle PRIVATE ' + quote(repo / 'runtime') + ' ' + quote(root) + ' ' + quote(directory) + ')',
         'target_compile_definitions(state_oracle PRIVATE PYC_SPLIT_MODEL)',
         'target_precompile_headers(state_oracle PRIVATE ' + ' '.join(map(quote, pch)) + ')']
libraries = manifest.get('runtime', {}).get('library_files', [])
if libraries:
    lines.append('target_link_libraries(state_oracle PRIVATE ' + ' '.join(map(quote, libraries)) + ')')
(root / 'CMakeLists.txt').write_text('\n'.join(lines) + '\n')
PY
cmake -S "${gate_dir}" -B "${gate_dir}/build" -DCMAKE_CXX_COMPILER="${cxx}" \
  -DCMAKE_BUILD_TYPE=Release '-DCMAKE_CXX_FLAGS_RELEASE=-O3 -DNDEBUG' \
  > "${gate_dir}/configure.log" 2>&1 || { cat "${gate_dir}/configure.log"; exit 1; }
compile_bounded split_pch cmake --build "${gate_dir}/build" --parallel 1
rg -q 'cmake_pch' "${gate_dir}/split_pch.build.log"
PYC_SIM_STATS=1 "${gate_dir}/build/state_oracle" > "${gate_dir}/split.oracle"
cat "${gate_dir}/split.oracle"
cmp "${gate_dir}/direct.oracle" "${gate_dir}/split.oracle"

# The standalone --out-dir/--cpp-split=none preamble must also define helper
# attributes. 258 state actions crosses the 256 threshold without recompiling
# the full 1024-register direct case for a third time.
python3 "${repo_root}/tests/newcircuit/generate_large_state_probe.py" "${gate_dir}/none" --count 258
"${PYCC}" "${gate_dir}/none/large_state_probe.mlir" --emit=cpp --cpp-split=none \
  --out-dir="${gate_dir}/none"
python3 - "${gate_dir}/none/top.hpp" <<'PYSTRUCT'
from pathlib import Path
import sys
source = Path(sys.argv[1]).read_text()
for group in range(2):
    assert f'void PYC_NOINLINE tick_reset_group_{group}_compute()' in source
    for branch in ('reset', 'data', 'noedge', 'negedge'):
        assert f'inline void tick_reset_group_{group}_{branch}_part_0()' in source
for part in range(2):
    assert f'void PYC_NOINLINE tick_local_commit_part_{part}()' in source
assert 'inline void tick_local_compute_part_0()' in source
print('Mixed small-reset/large-commit inlining boundary PASS')
PYSTRUCT
compile_bounded header_none "${cxx}" -std=c++17 -O3 -DPYC_SPLIT_MODEL \
  -I "${repo_root}/runtime" -I "${gate_dir}/none" "${harness}" -o "${gate_dir}/none-oracle"
PYC_SIM_STATS=1 "${gate_dir}/none-oracle" > "${gate_dir}/none.oracle"
cat "${gate_dir}/none.oracle"

"${PYCC}" "${gate_dir}/large_state_probe.mlir" --emit=verilog -o "${gate_dir}/large_state_probe.sv"
iverilog -g2012 -s large_state_probe_tb -I "${repo_root}/runtime/verilog" \
  -o "${gate_dir}/rtl-oracle" "${gate_dir}/large_state_probe.sv" "${gate_dir}/large_state_probe_tb.sv"
vvp "${gate_dir}/rtl-oracle" > "${gate_dir}/rtl.oracle"
cat "${gate_dir}/rtl.oracle"
cmp "${gate_dir}/direct.oracle" "${gate_dir}/rtl.oracle"
python3 - "${gate_dir}" <<'PY'
from pathlib import Path
import sys
root = Path(sys.argv[1])
for n, names in [(1024, ['direct.oracle', 'split.oracle', 'rtl.oracle']), (258, ['none.oracle'])]:
    state = [0] * n
    digest = 1469598103934665603
    for cycle in range(96):
        reset = [cycle % 17 == 0, cycle % 23 == 0]
        enable = [cycle % 3 != 0, cycle % 5 != 0, cycle % 7 != 0, True]
        data = (cycle * 977 + 31) & 65535
        for i in range(n):
            if reset[i >= n // 2]: state[i] = (i * 13 + 7) & 65535
            elif enable[i % 4]: state[i] = (state[i] + data + i * 37 + 11) & 65535
            digest = ((digest ^ state[i]) * 1099511628211) & ((1 << 64) - 1)
    expected = f'Large state/probe oracle PASS: {n * 96} checks; digest={digest:016x}\n'
    for name in names:
        assert (root / name).read_text() == expected, name
PY
echo 'NewCircuit large state/probe chunk gate passed (direct/split+PCH/header-none/Verilog)'
