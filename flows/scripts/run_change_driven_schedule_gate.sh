#!/usr/bin/env bash
# Gate scaffold for the two-level change-driven scheduler.
set -euo pipefail

source "$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)/lib.sh"

run_id="${PYC_GATE_RUN_ID:-$(date +%Y%m%d-%H%M%S)}"
docs_dir="${PYC_ROOT_DIR}/docs/gates/logs/${run_id}"
smoke_script="${PYC_ROOT_DIR}/compiler/mlir/test/change_driven_schedule_stats_smoke.sh"
stats_file="${PYC_ROOT_DIR}/.pycircuit_out/gates/change_driven_schedule_stats_smoke/stats.jsonl"
mkdir -p "${docs_dir}"

cat >"${docs_dir}/commands.txt" <<EOF
bash flows/scripts/pyc build
bash compiler/mlir/test/change_driven_schedule_stats_smoke.sh
python3 flows/tools/perf/analyze_change_driven_stats.py ${stats_file}
EOF

status_build="pending"
status_smoke="pending"
status_parser="pending"

write_summary() {
  python3 - "${docs_dir}/summary.json" "${run_id}" \
    "${status_build}" "${status_smoke}" "${status_parser}" "${smoke_script}" <<'PY'
import json
import sys

out, run_id, build, smoke, parser, smoke_script = sys.argv[1:]
payload = {
    "run_id": run_id,
    "feature": "two-level-change-driven-scheduler",
    "decisions": ["0104", "0105", "0106", "0107", "0109", "0117", "0127"],
    "gates": {
        "pyc_build": {"status": build},
        "change_driven_schedule_stats_smoke": {
            "status": smoke,
            "expected_path": smoke_script,
        },
        "analyze_change_driven_stats": {"status": parser},
    },
}
with open(out, "w", encoding="utf-8") as stream:
    json.dump(payload, stream, indent=2, sort_keys=True)
    stream.write("\n")
PY
}

on_exit() {
  local rc=$?
  write_summary || true
  if [[ "${rc}" -eq 0 ]]; then
    pyc_log "ok: wrote ${docs_dir}/summary.json"
  else
    pyc_log "incomplete: wrote ${docs_dir}/summary.json (exit=${rc})"
  fi
  exit "${rc}"
}
trap on_exit EXIT

pyc_log "gate run-id=${run_id}"
pyc_log "gate evidence: ${docs_dir}"

if bash "${PYC_ROOT_DIR}/flows/scripts/pyc" build \
  >"${docs_dir}/pyc_build.stdout" \
  2>"${docs_dir}/pyc_build.stderr"; then
  status_build="pass"
else
  status_build="fail"
  status_smoke="blocked"
  status_parser="blocked"
  : >"${docs_dir}/change_driven_schedule_stats_smoke.stdout"
  : >"${docs_dir}/analyze_change_driven_stats.stdout"
  printf 'blocked: pyc build failed\n' \
    >"${docs_dir}/change_driven_schedule_stats_smoke.stderr"
  printf 'blocked: pyc build failed\n' \
    >"${docs_dir}/analyze_change_driven_stats.stderr"
  exit 1
fi

if [[ ! -f "${smoke_script}" ]]; then
  status_smoke="missing"
  status_parser="blocked"
  : >"${docs_dir}/change_driven_schedule_stats_smoke.stdout"
  : >"${docs_dir}/analyze_change_driven_stats.stdout"
  printf 'missing required scheduler smoke test: %s\n' "${smoke_script}" \
    >"${docs_dir}/change_driven_schedule_stats_smoke.stderr"
  printf 'blocked: smoke must produce %s\n' "${stats_file}" \
    >"${docs_dir}/analyze_change_driven_stats.stderr"
  exit 2
fi

if PYCC="$(pyc_find_pycc)" bash "${smoke_script}" \
  >"${docs_dir}/change_driven_schedule_stats_smoke.stdout" \
  2>"${docs_dir}/change_driven_schedule_stats_smoke.stderr"; then
  status_smoke="pass"
else
  status_smoke="fail"
  status_parser="blocked"
  : >"${docs_dir}/analyze_change_driven_stats.stdout"
  printf 'blocked: scheduler smoke failed\n' \
    >"${docs_dir}/analyze_change_driven_stats.stderr"
  exit 1
fi

if [[ ! -f "${stats_file}" ]]; then
  status_parser="blocked"
  : >"${docs_dir}/analyze_change_driven_stats.stdout"
  printf 'blocked: smoke did not produce expected stats file: %s\n' "${stats_file}" \
    >"${docs_dir}/analyze_change_driven_stats.stderr"
  exit 2
fi

if python3 "${PYC_ROOT_DIR}/flows/tools/perf/analyze_change_driven_stats.py" \
  --out-json "${docs_dir}/change-driven-summary.json" \
  --out-md "${docs_dir}/change-driven-summary.md" \
  "${stats_file}" \
  >"${docs_dir}/analyze_change_driven_stats.stdout" \
  2>"${docs_dir}/analyze_change_driven_stats.stderr"; then
  status_parser="pass"
else
  status_parser="fail"
  exit 1
fi
