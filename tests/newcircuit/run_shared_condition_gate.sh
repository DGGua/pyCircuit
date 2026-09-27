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
python3 "${repo_root}/tests/newcircuit/shared_condition_oracle.py" prepare "${gate_dir}"
for fixture in late late_tree select dependent effect; do
  for mode in default bounded strict preserve; do
    options=()
    case "${mode}" in
      bounded) options+=(--sim-supernode-max-size=1) ;;
      strict) options+=(--sim-supernode-strict-bound=true --sim-supernode-max-size=24) ;;
      preserve) options+=(--sim-mode=cpp-only --cpp-only-preserve-ops) ;;
    esac
    "${PYCC}" "${gate_dir}/${fixture}.mlir" --emit=cpp --logic-depth=64 \
      "${options[@]}" -o "${gate_dir}/${fixture}_${mode}.cpp"
  done
done
python3 "${repo_root}/tests/newcircuit/shared_condition_oracle.py" check "${gate_dir}"
for fixture in late late_tree select dependent effect; do
  extra=()
  case "${fixture}" in
    late_tree) extra+=(-DINLINE_TREE) ;;
    dependent) extra+=(-DDEPENDENT_MUX) ;;
    effect) extra+=(-DEFFECT_BARRIER) ;;
  esac
  for mode in default bounded strict preserve; do
    variant="${fixture}_${mode}"
    "${CXX:-c++}" -std=c++17 -I "${PYC_TOOLCHAIN_ROOT}/include" -I "${gate_dir}" \
      "-DGENERATED_CIRCUIT=\"${variant}.cpp\"" "${extra[@]}" \
      "${repo_root}/tests/newcircuit/shared_condition_harness.cpp" -o "${gate_dir}/${variant}"
    "${gate_dir}/${variant}"
  done
done
echo 'shared-condition functional oracle: default, bounded, strict and preserve-ops passed'
