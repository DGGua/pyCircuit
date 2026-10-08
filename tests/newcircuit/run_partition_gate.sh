#!/usr/bin/env bash
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
python3 "${repo_root}/tests/newcircuit/partition_oracle.py" prepare "${gate_dir}"
common=(--emit=cpp --sim-expression-inlining=false --sim-replication=false --logic-depth=20000)
"${PYCC}" "${gate_dir}/atomic.mlir" "${common[@]}" \
  --sim-supernode-max-size=2 -o "${gate_dir}/atomic.cpp"
"${PYCC}" "${gate_dir}/atomic.mlir" "${common[@]}" \
  --sim-supernode-max-size=2 --sim-supernode-strict-bound=true \
  -o "${gate_dir}/atomic_strict.cpp"
"${PYCC}" "${gate_dir}/cost.mlir" "${common[@]}" \
  --sim-supernode-max-size=3 -o "${gate_dir}/cost.cpp"
for fixture in siblings31 sibling_hash siblings_overflow limit7001 limit7002; do
  "${PYCC}" "${gate_dir}/${fixture}.mlir" "${common[@]}" \
    --sim-supernode-max-size=0 -o "${gate_dir}/${fixture}.cpp"
done
python3 "${repo_root}/tests/newcircuit/partition_oracle.py" check "${gate_dir}"
for fixture in atomic cost; do
  "${PYCC}" "${gate_dir}/${fixture}.mlir" "${common[@]}" \
    --sim-mode=cpp-only --cpp-only-preserve-ops -o "${gate_dir}/${fixture}_preserve.cpp"
done
for variant in atomic atomic_strict atomic_preserve cost cost_preserve; do
  extra=()
  if [[ "${variant}" == cost* ]]; then extra+=(-DCOST_DAG); fi
  "${CXX:-c++}" -std=c++17 -I "${PYC_TOOLCHAIN_ROOT}/include" -I "${gate_dir}" \
    "-DGENERATED_CIRCUIT=\"${variant}.cpp\"" "${extra[@]}" \
    "${repo_root}/tests/newcircuit/partition_harness.cpp" -o "${gate_dir}/${variant}"
  "${gate_dir}/${variant}"
done
echo 'GSIM partition functional oracle: default, strict and preserve-ops passed'
