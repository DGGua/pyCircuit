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
python3 "${repo_root}/tests/newcircuit/generate_replication_cases.py" "${gate_dir}"
fixtures=(compound repeated named fanout wide_input array_compound array_row wide_result state mux_compound div_compound)
for case_id in "${!fixtures[@]}"; do
  fixture="${fixtures[case_id]}"
  for mode in on off; do
    bound=2
    [[ "${fixture}" == array_row || "${fixture}" == wide_input || "${fixture}" == wide_result ]] && bound=1
    args=(--sim-supernode-max-size="${bound}" --sim-supernode-strict-bound=true)
    [[ "${mode}" == off ]] && args+=(--sim-replication=false)
    cpp="${gate_dir}/${fixture}_${mode}.cpp"
    "${PYCC}" "${gate_dir}/${fixture}.mlir" --emit=cpp "${args[@]}" -o "${cpp}"
    case "${fixture}" in
      compound|array_compound|state|mux_compound|div_compound)
        root=2
        [[ "${fixture}" == state ]] && root=3
        if [[ "${mode}" == on ]]; then
          if rg -q "^[[:space:]]+pyc_add_${root} = " "${cpp}"; then
            echo "compound root was not replicated: ${fixture}" >&2
            exit 1
          fi
        else
          rg -q "^[[:space:]]+pyc_add_${root} = " "${cpp}"
        fi ;;
      repeated|fanout) rg -q '^[[:space:]]+pyc_add_2 = ' "${cpp}" ;;
      named) rg -q '^[[:space:]]+anchor = ' "${cpp}" ;;
      wide_input)
        if [[ "${mode}" == on ]]; then
          if rg -q '^[[:space:]]+pyc_extract_[0-9]+ = ' "${cpp}"; then
            echo 'wide materialized input prevented a scalar copy' >&2
            exit 1
          fi
        else
          rg -q '^[[:space:]]+pyc_extract_[0-9]+ = ' "${cpp}"
        fi ;;
      array_row)
        expected=1
        [[ "${mode}" == off ]] && expected=2
        test "$(rg -c '^[[:space:]]+pyc_v_get_[0-9]+ = ' "${cpp}")" = "${expected}" ;;
      wide_result) rg -q '^[[:space:]]+pyc_xor_1 = ' "${cpp}" ;;
    esac
    cp "${cpp}" "${gate_dir}/replication_under_test.cpp"
    "${CXX:-c++}" -std=c++17 -I "${PYC_TOOLCHAIN_ROOT}/include" -I "${gate_dir}" \
      -DCASE_ID="${case_id}" "${repo_root}/tests/newcircuit/replication_dag_harness.cpp" \
      -o "${gate_dir}/${fixture}_${mode}"
    "${gate_dir}/${fixture}_${mode}" > "${gate_dir}/${fixture}_${mode}.out"
  done
  cmp "${gate_dir}/${fixture}_on.out" "${gate_dir}/${fixture}_off.out"
  echo "PASS replication ${fixture} (on/off, 4096 vectors/cycles)"
done
# The strict partition above isolates cost decisions. Also run the same
# oracles through the default atomic GSIM partition policy.
for fixture in compound state; do
  case_id=0
  [[ "${fixture}" == state ]] && case_id=8
  for mode in on off; do
    args=()
    [[ "${mode}" == off ]] && args+=(--sim-replication=false)
    "${PYCC}" "${gate_dir}/${fixture}.mlir" --emit=cpp "${args[@]}" \
      -o "${gate_dir}/replication_under_test.cpp"
    "${CXX:-c++}" -std=c++17 -I "${PYC_TOOLCHAIN_ROOT}/include" -I "${gate_dir}" \
      -DCASE_ID="${case_id}" "${repo_root}/tests/newcircuit/replication_dag_harness.cpp" \
      -o "${gate_dir}/default_${fixture}_${mode}"
    "${gate_dir}/default_${fixture}_${mode}" > "${gate_dir}/default_${fixture}_${mode}.out"
    cmp "${gate_dir}/${fixture}_${mode}.out" "${gate_dir}/default_${fixture}_${mode}.out"
  done
  echo "PASS replication ${fixture} (default partition, on/off, 4096 vectors/cycles)"
done
echo 'PASS NewCircuit replication gate'
