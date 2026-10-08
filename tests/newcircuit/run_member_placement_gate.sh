#!/usr/bin/env bash
# Placement is always planned from the NewCircuit graph and final chunks.
set -euo pipefail
repo_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)"
: "${PYCC:?set PYCC to the compiler under test}"
gate_dir="${PYC_MEMBER_PLACEMENT_GATE_DIR:-$(mktemp -d)}"
mkdir -p "${gate_dir}"
gate_dir="$(realpath "${gate_dir}")"
if [[ -z "${PYC_MEMBER_PLACEMENT_GATE_DIR:-}" ]]; then trap 'rm -rf -- "${gate_dir}"' EXIT; fi
"${PYCC}" --help > "${gate_dir}/help.txt"
if rg -q -- '--cpp-localize-members' "${gate_dir}/help.txt"; then
  echo 'placement must be always-on, not a user-facing optional semantic path' >&2
  exit 1
fi
rg -q -- '--cpp-compile-budget' "${gate_dir}/help.txt"
fixture="${repo_root}/tests/newcircuit/member_placement.mlir"
harness="${repo_root}/tests/newcircuit/member_placement_harness.cpp"
common=(--emit=cpp --sim-expression-inlining=false --sim-replication=false --sim-supernode-max-size=64)
for mode in flat crosschunk nested; do
  mkdir -p "${gate_dir}/${mode}"
  args=()
  [[ "${mode}" != crosschunk ]] || args+=(--cpp-shard-max-ast-nodes=2)
  [[ "${mode}" != nested ]] || args+=(--cpp-shard-max-ast-nodes=3)
  "${PYCC}" "${fixture}" "${common[@]}" "${args[@]}" -o "${gate_dir}/${mode}/member_placement.cpp"
  "${PYCC}" "${fixture}" "${common[@]}" "${args[@]}" --cpp-split=module --cpp-pch \
    --cpp-compile-budget=false --out-dir="${gate_dir}/${mode}/split" \
    --cpp-manifest="${gate_dir}/${mode}/split/manifest.json"
done
python3 "${repo_root}/tests/newcircuit/check_member_placement.py" "${gate_dir}"
for mode in flat crosschunk nested; do
  "${CXX:-clang++}" -std=c++17 -O2 -I "${repo_root}/runtime" -I "${gate_dir}/${mode}" \
    "${harness}" -o "${gate_dir}/${mode}/oracle"
  PYC_SIM_STATS=1 "${gate_dir}/${mode}/oracle" > "${gate_dir}/${mode}/oracle.log"
  cat "${gate_dir}/${mode}/oracle.log"
done
# Repeat the exact chunked lowering and compare only deterministic code files.
"${PYCC}" "${fixture}" "${common[@]}" --cpp-shard-max-ast-nodes=2 \
  --cpp-split=module --cpp-pch --cpp-compile-budget=false \
  --out-dir="${gate_dir}/repeat" --cpp-manifest="${gate_dir}/repeat/manifest.json"
python3 - "${gate_dir}" "${repo_root}" <<'PY'
import json
from pathlib import Path
import sys
root, repo = map(Path, sys.argv[1:])
directory = root / 'crosschunk/split'
repeat = root / 'repeat'
for file in directory.iterdir():
    if file.suffix in ('.cpp', '.hpp'):
        assert file.read_bytes() == (repeat / file.name).read_bytes(), file.name
manifest = json.loads((directory / 'manifest.json').read_text())
pch = manifest.get('precompile_headers')
assert manifest.get('precompile_headers_mode') == 'device_hpp' and pch
assert all(Path(p).is_file() for p in pch)
sources = [(directory / source['path']).resolve() for source in manifest['sources']]
sources.append(repo / 'tests/newcircuit/member_placement_harness.cpp')
quote = lambda path: json.dumps(str(path))
lines = ['cmake_minimum_required(VERSION 3.20)', 'project(member_placement LANGUAGES CXX)',
         'set(CMAKE_CXX_STANDARD 17)', 'set(CMAKE_CXX_STANDARD_REQUIRED ON)',
         'add_executable(placement_oracle ' + ' '.join(map(quote, sources)) + ')',
         'target_include_directories(placement_oracle PRIVATE ' + quote(repo / 'runtime') + ' ' + quote(directory) + ')',
         'target_compile_definitions(placement_oracle PRIVATE PYC_SPLIT_MODEL)',
         'target_precompile_headers(placement_oracle PRIVATE ' + ' '.join(map(quote, pch)) + ')']
libraries = manifest.get('runtime', {}).get('library_files', [])
if libraries:
    lines.append('target_link_libraries(placement_oracle PRIVATE ' + ' '.join(map(quote, libraries)) + ')')
(root / 'CMakeLists.txt').write_text('\n'.join(lines) + '\n')
PY
cmake -S "${gate_dir}" -B "${gate_dir}/build" -DCMAKE_CXX_COMPILER="${CXX:-clang++}" \
  -DCMAKE_BUILD_TYPE=Release > "${gate_dir}/configure.log" 2>&1 || { cat "${gate_dir}/configure.log"; exit 1; }
cmake --build "${gate_dir}/build" --parallel 1 > "${gate_dir}/build.log" 2>&1 || { cat "${gate_dir}/build.log"; exit 1; }
rg -q 'cmake_pch' "${gate_dir}/build.log"
PYC_SIM_STATS=1 "${gate_dir}/build/placement_oracle" > "${gate_dir}/split-pch.log"
cat "${gate_dir}/split-pch.log"
python3 - "${gate_dir}" <<'PY'
from pathlib import Path
import sys
root = Path(sys.argv[1])
expected = 'Member placement oracle PASS: 2048 cases; digest=d83780b64c6057a2; probes=6dcb4591733b2428\n'
for path in [root / mode / 'oracle.log' for mode in ('flat', 'crosschunk', 'nested')] + [root / 'split-pch.log']:
    assert path.read_text() == expected, path
PY
# Observation effects are legal comb metadata; hardware effects remain illegal.
if "${PYCC}" "${repo_root}/tests/newcircuit/impure_comb.pyc" --emit=cpp \
    -o "${gate_dir}/invalid.cpp" > "${gate_dir}/invalid.log" 2>&1; then
  echo 'side-effecting comb unexpectedly accepted' >&2
  exit 1
fi
rg -q 'memory-effect-free|hardware-pure|side effect|must contain only pure operations' "${gate_dir}/invalid.log"
"${PYCC}" "${repo_root}/tests/newcircuit/member_placement_cuts.mlir" "${common[@]}" \
  --cpp-shard-max-ast-nodes=4 --cpp-split=module --out-dir="${gate_dir}/cuts" \
  --cpp-manifest="${gate_dir}/cuts/manifest.json"
python3 "${repo_root}/tests/newcircuit/check_member_placement_cuts.py" "${gate_dir}/cuts" \
  --baseline "${repo_root}/tests/newcircuit/member_placement_cuts_baseline.json"
"${CXX:-clang++}" -std=c++17 -O2 -I "${repo_root}/runtime" -I "${gate_dir}/cuts" \
  "${gate_dir}"/cuts/*.cpp "${repo_root}/tests/newcircuit/member_placement_cuts_harness.cpp" \
  -o "${gate_dir}/cuts/oracle"
"${gate_dir}/cuts/oracle" > "${gate_dir}/cuts/oracle.log"
cat "${gate_dir}/cuts/oracle.log"
echo 'NewCircuit member placement gate passed (locals/chunks/nested/probes/activity/direct/split+PCH)'
