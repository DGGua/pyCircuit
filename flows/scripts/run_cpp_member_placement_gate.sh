#!/usr/bin/env bash
set -euo pipefail

source "$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)/lib.sh"

run_id="${PYC_GATE_RUN_ID:-$(date +%Y%m%d-%H%M%S)}"
docs_dir="${PYC_ROOT_DIR}/docs/gates/logs/${run_id}"
mkdir -p "${docs_dir}"

cat >"${docs_dir}/commands.txt" <<EOF
bash flows/scripts/pyc build
bash compiler/mlir/test/cpp_member_placement_smoke.sh
EOF

pyc_log "gate run-id=${run_id}"
pyc_log "docs evidence: ${docs_dir}"

bash "${PYC_ROOT_DIR}/flows/scripts/pyc" build \
  >"${docs_dir}/pyc_build.stdout" 2>"${docs_dir}/pyc_build.stderr"

PYCC="$(pyc_find_pycc)" bash "${PYC_ROOT_DIR}/compiler/mlir/test/cpp_member_placement_smoke.sh" \
  >"${docs_dir}/cpp_member_placement_smoke.stdout" \
  2>"${docs_dir}/cpp_member_placement_smoke.stderr"

python3 - <<'PY' "${docs_dir}/summary.json" "${run_id}"
import json
import sys

out, run_id = sys.argv[1], sys.argv[2]
json.dump(
    {
        "run_id": run_id,
        "gates": {
            "pyc_build": {"status": "pass"},
            "cpp_member_placement_smoke": {"status": "pass"},
        },
        "decisions": ["0141", "0147"],
        "feature": "cpp-localize-members",
    },
    open(out, "w", encoding="utf-8"),
    indent=2,
)
PY

pyc_log "ok: wrote ${docs_dir}/summary.json"
