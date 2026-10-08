#!/usr/bin/env bash
set -euo pipefail
repo_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)"
PYCC="${PYCC:-${repo_root}/.pycircuit_out/toolchain/build/bin/pycc}"
gate_dir="$(mktemp -d)"
trap 'rm -rf -- "${gate_dir}"' EXIT
fixture="${repo_root}/tests/newcircuit/vector_instance_cache.mlir"
harness="${repo_root}/tests/newcircuit/vector_instance_cache_harness.cpp"
"${PYCC}" "${fixture}" --emit=cpp --hierarchical --noinline \
  -o "${gate_dir}/vector_instance_cache.cpp"
# Six i8 lanes use value equality; six i1024 lanes require 6 * 16 words.
rg -Fq 'std::array<std::uint64_t, 96> u_wide_eval_cache_words' "${gate_dir}/vector_instance_cache.cpp"
rg -Fq 'u_small_eval_cache_in_0' "${gate_dir}/vector_instance_cache.cpp"
! rg -q 'u_small_eval_cache_(words|in_fp_)' "${gate_dir}/vector_instance_cache.cpp"
for mode in cached uncached; do
  defs=()
  [[ "${mode}" == uncached ]] && defs+=(-DPYC_DISABLE_INSTANCE_EVAL_CACHE)
  "${CXX:-c++}" -std=c++17 -O1 -I "${repo_root}/runtime" -I "${gate_dir}" \
    "${defs[@]}" "${harness}" -o "${gate_dir}/harness"
  PYC_SIM_STATS=1 "${gate_dir}/harness"
done

# Exercise the actual split-module manifest and its requested device PCH.
"${PYCC}" "${fixture}" --emit=cpp --hierarchical --noinline --cpp-split=module --cpp-pch \
  --out-dir="${gate_dir}/split" --cpp-manifest="${gate_dir}/split/manifest.json"
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
assert len(sources) >= 3, 'hierarchy was unexpectedly inlined'
sources.append(repo / 'tests/newcircuit/vector_instance_cache_harness.cpp')
quote = lambda path: json.dumps(str(path))
lines = ['cmake_minimum_required(VERSION 3.20)', 'project(vector_instance_cache LANGUAGES CXX)',
         'set(CMAKE_CXX_STANDARD 17)', 'set(CMAKE_CXX_STANDARD_REQUIRED ON)',
         'add_executable(cache_oracle ' + ' '.join(map(quote, sources)) + ')',
         'target_include_directories(cache_oracle PRIVATE ' + quote(repo / 'runtime') + ' ' + quote(directory) + ')',
         'target_compile_definitions(cache_oracle PRIVATE PYC_SPLIT_MODEL)',
         'target_precompile_headers(cache_oracle PRIVATE ' + ' '.join(map(quote, pch)) + ')']
libraries = manifest.get('runtime', {}).get('library_files', [])
if libraries:
    lines.append('target_link_libraries(cache_oracle PRIVATE ' + ' '.join(map(quote, libraries)) + ')')
(root / 'CMakeLists.txt').write_text('\n'.join(lines) + '\n')
PY
cmake -S "${gate_dir}" -B "${gate_dir}/build" -DCMAKE_CXX_COMPILER="${CXX:-c++}" \
  -DCMAKE_BUILD_TYPE=Release > "${gate_dir}/configure.log" 2>&1 || { cat "${gate_dir}/configure.log"; exit 1; }
cmake --build "${gate_dir}/build" --parallel 2 > "${gate_dir}/build.log" 2>&1 || { cat "${gate_dir}/build.log"; exit 1; }
rg -q 'cmake_pch' "${gate_dir}/build.log"
PYC_SIM_STATS=1 "${gate_dir}/build/cache_oracle"
echo 'NewCircuit vector instance cache gate passed'
