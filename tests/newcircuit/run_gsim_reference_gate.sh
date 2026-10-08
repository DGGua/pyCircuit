#!/usr/bin/env bash
# Optional, offline differential reference. Supply an already-built GSIM binary.
# GSIM=/path/to/gsim GSIM_REFERENCE_CXX=clang++-19 ./run_gsim_reference_gate.sh
# GCC is limited to this <=64-bit fixture and requires explicit opt-in:
# GSIM_REFERENCE_CXX=g++ GSIM_REFERENCE_ALLOW_GCC_U64=1
set -euo pipefail
repo_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)"
source "${repo_root}/flows/scripts/lib.sh"
[[ -n "${GSIM:-}" && -x "${GSIM}" ]] || pyc_die "set GSIM to an existing reference compiler executable"
GSIM="$(realpath -- "${GSIM}")"
if [[ -z "${PYCC:-}" && -x "${repo_root}/.pycircuit_out/toolchain/build/bin/pycc" ]]; then
  PYCC="${repo_root}/.pycircuit_out/toolchain/build/bin/pycc"
fi
pyc_find_pycc
if [[ ! -d "${PYC_TOOLCHAIN_ROOT}/include/cpp" ]]; then
  PYC_TOOLCHAIN_ROOT="${repo_root}/.pycircuit_out/toolchain/install"
fi
gate_dir="$(mktemp -d)"
trap 'rm -rf -- "${gate_dir}"' EXIT
reference_cxx="${GSIM_REFERENCE_CXX:-clang++-19}"
command -v "${reference_cxx}" >/dev/null || pyc_die "reference model compiler not found: ${reference_cxx}"
"${reference_cxx}" --version > "${gate_dir}/compiler-version.txt"
head -n 1 "${gate_dir}/compiler-version.txt"
"${reference_cxx}" -dM -E -x c++ /dev/null > "${gate_dir}/compiler-macros.txt"
reference_flags=(-O2 -std=c++17)
if [[ "${GSIM_REFERENCE_ALLOW_GCC_U64:-0}" == 1 ]]; then
  if rg -q '^#define __clang__ ' "${gate_dir}/compiler-macros.txt" ||
     ! rg -q '^#define __GNUC__ ' "${gate_dir}/compiler-macros.txt"; then
    pyc_die "GCC u64 opt-in requires a GCC compiler"
  fi
  echo "NON-OFFICIAL GCC reference model: -O2 -std=c++17 -D__BITINT_MAXWIDTH__=64"
  echo "Guard bypass is limited to generated files verified to contain no _BitInt."
  reference_flags+=(-D__BITINT_MAXWIDTH__=64)
else
  python3 - "${gate_dir}/compiler-macros.txt" <<'PY'
import pathlib, re, sys
macros = pathlib.Path(sys.argv[1]).read_text()
major = re.search(r'^#define __clang_major__ (\d+)$', macros, re.M)
assert major and int(major[1]) >= 19, 'official GSIM model requires Clang >=19; GCC needs explicit u64 opt-in'
assert re.search(r'^#define __BITINT_MAXWIDTH__ ', macros, re.M), 'missing _BitInt support'
PY
fi
"${GSIM}" --version
mkdir "${gate_dir}/gsim"
"${GSIM}" --dir="${gate_dir}/gsim" --log-level=1 \
  "${repo_root}/tests/newcircuit/gsim_reference.fir" > "${gate_dir}/generate.log" 2>&1 || {
  cat "${gate_dir}/generate.log"
  exit 1
}
if [[ "${GSIM_REFERENCE_ALLOW_GCC_U64:-0}" == 1 ]]; then
  python3 - "${gate_dir}/gsim" <<'PY'
import pathlib, re, sys
files = [p for p in pathlib.Path(sys.argv[1]).iterdir() if p.suffix in ('.h', '.cpp')]
assert files, 'missing GSIM generated sources'
for path in files:
    assert not re.search(r'\b_BitInt\b', path.read_text()), f'GCC u64 fallback cannot compile {path}: _BitInt required'
PY
fi
"${reference_cxx}" "${reference_flags[@]}" -I "${gate_dir}/gsim" \
  "${gate_dir}"/gsim/SmallReference*.cpp \
  "${repo_root}/tests/newcircuit/gsim_reference_harness.cpp" -o "${gate_dir}/gsim-oracle"
"${PYCC}" "${repo_root}/tests/newcircuit/gsim_reference.mlir" \
  --emit=cpp -o "${gate_dir}/gsim_reference.cpp"
"${CXX:-c++}" -O2 -std=c++17 -DGSIM_REFERENCE_NEWCIRCUIT \
  -I "${gate_dir}" -I "${PYC_TOOLCHAIN_ROOT}/include" \
  "${repo_root}/tests/newcircuit/gsim_reference_harness.cpp" -o "${gate_dir}/newcircuit-oracle"
"${gate_dir}/gsim-oracle" > "${gate_dir}/gsim.log"
"${gate_dir}/newcircuit-oracle" > "${gate_dir}/newcircuit.log"
cat "${gate_dir}/gsim.log" "${gate_dir}/newcircuit.log"
cmp "${gate_dir}/gsim.log" "${gate_dir}/newcircuit.log"
rg -qx 'Arithmetic/mux/lowbits oracle PASS: 197632 checks; digest=e4ccd8d72edf7475' "${gate_dir}/gsim.log"
echo "GSIM / NewCircuit reference differential gate PASS (shared independent oracle and digest)"
