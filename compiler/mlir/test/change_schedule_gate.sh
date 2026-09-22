#!/usr/bin/env bash
# Gate for the phase-one canonical graph and change-driven schedule contract.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
PYCC="${PYCC:-${ROOT}/.pycircuit_out/toolchain/build/bin/pycc}"
PYC_OPT="${PYC_OPT:-${ROOT}/.pycircuit_out/toolchain/build/bin/pyc-opt}"
INPUT="${ROOT}/compiler/mlir/test/Inputs/change_schedule_chain.mlir"
OUT="${ROOT}/.pycircuit_out/gates/change_schedule"

if [[ ! -x "${PYCC}" ]]; then
  echo "fail: pycc not built at ${PYCC}" >&2
  exit 1
fi

rm -rf "${OUT}"
mkdir -p "${OUT}/first" "${OUT}/second"

run_plan() {
  local dump_dir=$1
  "${PYCC}" "${INPUT}" --emit=none -o /dev/null \
    --dump-pass-ir="${dump_dir}" --dump-pass-ir-phase=after \
    --dump-pass-ir-filter='change-driven-schedule' >/dev/null
}

run_plan "${OUT}/first"
run_plan "${OUT}/second"

first_plan=("${OUT}/first"/*_after_*plan-change-driven-schedule*.mlir)
second_plan=("${OUT}/second"/*_after_*plan-change-driven-schedule*.mlir)
if [[ ${#first_plan[@]} -ne 1 || ${#second_plan[@]} -ne 1 ]]; then
  echo "fail: expected one planner dump per run" >&2
  exit 1
fi
cmp "${first_plan[0]}" "${second_plan[0]}"

python3 - "${first_plan[0]}" <<'PY'
import re
import sys

text = open(sys.argv[1], encoding="utf-8").read()
summaries = re.findall(r"pyc\.change_schedule\.summary = \{([^}]+)\}", text)
if len(summaries) != 2:
    raise SystemExit(f"fail: expected 2 function summaries, got {len(summaries)}")
if 'node_count = 1 : i64' not in summaries[0]:
    raise SystemExit("fail: child summary node count is not 1")
if 'node_count = 3 : i64' not in summaries[1]:
    raise SystemExit("fail: top summary node count is not 3")
if 'edge_count = 2 : i64' not in summaries[1]:
    raise SystemExit("fail: top summary edge count is not 2")
if 'rank_count = 3 : i64' not in summaries[1]:
    raise SystemExit("fail: top summary rank count is not 3")

instance = next(
    (line for line in text.splitlines() if "pyc.instance" in line), None
)
if instance is None:
    raise SystemExit("fail: planned IR has no instance")
for expected in (
    "pyc.change_schedule.node = [1]",
    "pyc.change_schedule.rank = [1]",
    "pyc.change_schedule.slot = [1]",
    "pyc.change_schedule.fanout = [[2]]",
):
    if expected not in instance:
        raise SystemExit(f"fail: instance missing {expected!r}")

node_attrs = re.findall(r"pyc\.change_schedule\.node = \[[^\]]+\]", text)
if len(node_attrs) != 4:
    raise SystemExit(f"fail: expected 4 scheduled nodes, got {len(node_attrs)}")
print("ok: canonical schedule metadata is stable and verified")
PY

if [[ -x "${PYC_OPT}" ]]; then
  "${PYC_OPT}" "${INPUT}" \
    --pyc-plan-change-driven-schedule \
    --pyc-check-change-driven-schedule -o /dev/null

  expect_failure() {
    local input=$1
    local pass=$2
    local diagnostic=$3
    local log="${OUT}/$(basename "${input}").log"
    if "${PYC_OPT}" "${input}" "${pass}" -o /dev/null >"${log}" 2>&1; then
      echo "fail: expected ${input} to be rejected" >&2
      exit 1
    fi
    if ! grep -q "${diagnostic}" "${log}"; then
      echo "fail: ${input} did not report '${diagnostic}'" >&2
      cat "${log}" >&2
      exit 1
    fi
  }

  expect_failure \
    "${ROOT}/compiler/mlir/test/Inputs/change_schedule_missing_metadata.mlir" \
    --pyc-check-change-driven-schedule \
    "missing change-driven schedule summary"
  expect_failure \
    "${ROOT}/compiler/mlir/test/Inputs/change_schedule_tampered.mlir" \
    --pyc-check-change-driven-schedule \
    "schedule fanout size mismatch"
  expect_failure \
    "${ROOT}/compiler/mlir/test/Inputs/change_schedule_missing_summary.mlir" \
    --pyc-plan-change-driven-schedule \
    "missing combinational dependency summary"
  expect_failure \
    "${ROOT}/compiler/mlir/test/Inputs/change_schedule_multidriver.mlir" \
    --pyc-plan-change-driven-schedule \
    "multiple-driver wire"
  expect_failure \
    "${ROOT}/compiler/mlir/test/Inputs/change_schedule_cycle.mlir" \
    --pyc-plan-change-driven-schedule \
    "requires an acyclic"
  expect_failure \
    "${ROOT}/compiler/mlir/test/Inputs/change_schedule_unsupported.mlir" \
    --pyc-plan-change-driven-schedule \
    "unsupported result-producing op"
else
  echo "note: pyc-opt unavailable; standalone negative pass cases not run" >&2
fi

echo "ok: change-driven schedule gate passed"
