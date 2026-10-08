#!/usr/bin/env bash
# Gate for ESSENT-style comb partitioning: the partitioned emitter must produce
# bit-identical simulation traces to the default (identity-partition) emitter,
# and the generated code must expose partition-grouped evaluation.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
PYCC="${PYCC:-${ROOT}/.pycircuit_out/toolchain/build/bin/pycc}"
CXX="${CXX:-c++}"
INPUT="${ROOT}/compiler/mlir/test/Inputs/comb_dirty_scheduler.mlir"
DRIVER="${ROOT}/compiler/mlir/test/Inputs/comb_partition_trace_driver.cpp"
OUT="${ROOT}/.pycircuit_out/gates/comb_partition"

if [[ ! -x "${PYCC}" ]]; then
  echo "skip: pycc not built at ${PYCC}" >&2
  exit 0
fi

rm -rf "${OUT}"
mkdir -p "${OUT}/default" "${OUT}/part"

common=(--logic-depth=256 --build-profile=dev-fast
        --inline-policy=off --hierarchy-policy=strict)

"${PYCC}" "${INPUT}" --emit=cpp -o "${OUT}/default/comb_dirty_scheduler.cpp" "${common[@]}"
"${PYCC}" "${INPUT}" --emit=cpp -o "${OUT}/part/comb_dirty_scheduler.cpp" "${common[@]}" \
  --comb-partition-size=64

# Structural expectations for the partitioned build: all four dependent combs
# merge into one partition, the constant comb stays standalone.
grep -q 'eval_comb_part_0()' "${OUT}/part/comb_dirty_scheduler.cpp"
grep -q 'eval_comb_part_1()' "${OUT}/part/comb_dirty_scheduler.cpp"
if grep -q 'eval_comb_part_2()' "${OUT}/part/comb_dirty_scheduler.cpp"; then
  echo "fail: expected two comb partitions, found three" >&2
  exit 1
fi
grep -q '_pyc_part_0_inputs_valid' "${OUT}/part/comb_dirty_scheduler.cpp"
grep -q 'pyc::cpp::DirtyBitset<2>' "${OUT}/part/comb_dirty_scheduler.cpp"
grep -q '_pyc_comb_0_input_0' "${OUT}/part/comb_dirty_scheduler.cpp"
# The partition guard must compare the polled inputs of all member regions.
grep -q '_pyc_comb_0_input_0' "${OUT}/part/comb_dirty_scheduler.cpp"
grep -q '_pyc_comb_0_input_1' "${OUT}/part/comb_dirty_scheduler.cpp"
grep -q '_pyc_comb_2_input_1' "${OUT}/part/comb_dirty_scheduler.cpp"

for mode in default part; do
  "${CXX}" -std=c++17 -O0 \
    -I"${OUT}/${mode}" -I"${ROOT}/.pycircuit_out/toolchain/install/include" \
    "${DRIVER}" -o "${OUT}/run_${mode}"
  "${OUT}/run_${mode}" > "${OUT}/trace_${mode}.log"
done

if ! cmp "${OUT}/trace_default.log" "${OUT}/trace_part.log"; then
  echo "fail: partitioned simulation trace differs from default" >&2
  exit 1
fi

# Partition metadata contract on the IR.
dump_dir="${OUT}/ir"
"${PYCC}" "${INPUT}" --emit=none -o /dev/null "${common[@]}" \
  --comb-partition-size=64 \
  --dump-pass-ir="${dump_dir}" --dump-pass-ir-phase=after \
  --dump-pass-ir-filter='comb-partition' >/dev/null
python3 - "${dump_dir}"/*_after_*comb-partition*.mlir <<'PY'
import re
import sys

text = open(sys.argv[1], encoding="utf-8").read()
summary = re.search(r"partition_count = (\d+)", text)
if not summary:
    raise SystemExit("fail: partition_count missing from schedule summary")
if summary.group(1) != "2":
    raise SystemExit(f"fail: expected partition_count=2, got {summary.group(1)}")
parts = re.findall(r"pyc\.change_schedule\.partition = (\d+)", text)
if sorted(parts) != ["0", "0", "0", "0", "1"]:
    raise SystemExit(f"fail: unexpected partition assignment {parts}")
print("ok: comb partition metadata contract")
PY

echo "ok: comb partition smoke"
