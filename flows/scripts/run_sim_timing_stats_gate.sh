#!/usr/bin/env bash
# Gate wrapper for module fallback timing stats.
# Builds pycc, runs the smoke test, and writes evidence under
# docs/gates/logs/<run-id>/.
set -euo pipefail

source "$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)/lib.sh"

run_id="${PYC_GATE_RUN_ID:-$(date +%Y%m%d-%H%M%S)}"
docs_dir="${PYC_ROOT_DIR}/docs/gates/logs/${run_id}"
mkdir -p "${docs_dir}"

cat >"${docs_dir}/commands.txt" <<EOF
bash flows/scripts/pyc build
bash compiler/mlir/test/sim_timing_stats_smoke.sh
python3 flows/tools/perf/analyze_module_fallback_stats.py --require-timing .pycircuit_out/gates/sim_timing_stats_smoke/fallback.jsonl
EOF

pyc_log "gate run-id=${run_id}"
pyc_log "docs evidence: ${docs_dir}"

status_build="pending"
status_smoke="pending"
status_parser="pending"

write_summary() {
  python3 - <<'PY' "${docs_dir}/summary.json" "${run_id}" "${status_build}" "${status_smoke}" "${status_parser}"
import json
import sys

out, run_id, status_build, status_smoke, status_parser = sys.argv[1:6]
json.dump(
    {
        "run_id": run_id,
        "gates": {
            "pyc_build": {"status": status_build},
            "sim_timing_stats_smoke": {"status": status_smoke},
            "analyze_module_fallback_stats": {"status": status_parser},
        },
        "decisions": [],
        "feature": "module-fallback-timing-stats",
        "note": "measurement only; no semantic change",
    },
    open(out, "w", encoding="utf-8"),
    indent=2,
)
print(f"wrote {out}")
PY
}

on_exit() {
  local rc=$?
  if [[ "${status_build}" == "pending" && "${rc}" -ne 0 ]]; then
    status_build="fail"
  fi
  if [[ "${status_smoke}" == "pending" && "${rc}" -ne 0 ]]; then
    status_smoke="fail"
  fi
  if [[ "${status_parser}" == "pending" && "${rc}" -ne 0 ]]; then
    status_parser="fail"
  fi
  write_summary || true
  if [[ "${rc}" -eq 0 ]]; then
    pyc_log "ok: wrote ${docs_dir}/summary.json"
  else
    pyc_log "fail: wrote ${docs_dir}/summary.json (exit=${rc})"
  fi
  exit "${rc}"
}
trap on_exit EXIT

if bash "${PYC_ROOT_DIR}/flows/scripts/pyc" build \
  >"${docs_dir}/pyc_build.stdout" 2>"${docs_dir}/pyc_build.stderr"; then
  status_build="pass"
else
  status_build="fail"
  exit 1
fi

if PYCC="$(pyc_find_pycc)" bash "${PYC_ROOT_DIR}/compiler/mlir/test/sim_timing_stats_smoke.sh" \
  >"${docs_dir}/sim_timing_stats_smoke.stdout" \
  2>"${docs_dir}/sim_timing_stats_smoke.stderr"; then
  status_smoke="pass"
else
  status_smoke="fail"
  exit 1
fi

stats_file="${PYC_ROOT_DIR}/.pycircuit_out/gates/sim_timing_stats_smoke/fallback.jsonl"
if python3 "${PYC_ROOT_DIR}/flows/tools/perf/analyze_module_fallback_stats.py" \
  --require-timing \
  --out-json "${docs_dir}/fallback-summary.json" \
  --out-md "${docs_dir}/fallback-summary.md" \
  "${stats_file}" \
  >"${docs_dir}/analyze_module_fallback_stats.stdout" \
  2>"${docs_dir}/analyze_module_fallback_stats.stderr"; then
  status_parser="pass"
else
  status_parser="fail"
  exit 1
fi
