#!/usr/bin/env bash
# Applicable GSIM mechanisms plus the existing PYC hardware/runtime contract.
set -euo pipefail
repo_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)"
export PYCC="${PYCC:-${repo_root}/.pycircuit_out/toolchain/build/bin/pycc}"
export PYC_TOOLCHAIN_ROOT="${PYC_TOOLCHAIN_ROOT:-${repo_root}/.pycircuit_out/toolchain/install}"
run_id="${PYC_GATE_RUN_ID:-newcircuit-gsim-$(date +%Y%m%d-%H%M%S)}"
export PYC_GATE_RUN_ID="$run_id"
log_dir="${repo_root}/docs/gates/logs/${run_id}"
mkdir -p "$log_dir"
rm -f "${log_dir}/gsim_reproduction_summary.json"
gates=(simulation_plan partition replication shared_condition array_split
       scalar_demand scalar_fixedpoint activity primitive_activity)
for gate in "${gates[@]}"; do
  log="${log_dir}/${gate}.log"
  if ! bash "${repo_root}/tests/newcircuit/run_${gate}_gate.sh" >"$log" 2>&1; then
    tail -n 40 "$log" >&2
    echo "FAIL ${gate}: ${log}" >&2
    exit 1
  fi
  # Some Icarus versions print $fatal but return zero. Never accept a gate
  # whose simulator log reports a failed oracle even if its shell succeeded.
  if rg -n '^(FATAL|ERROR):|Traceback \(most recent call last\):' "$log"; then
    echo "FAIL ${gate}: simulator failure in ${log}" >&2
    exit 1
  fi
  echo "PASS ${gate}: ${log}"
done
if ! bash "${repo_root}/flows/scripts/run_semantic_regressions_v40.sh" \
    >"${log_dir}/semantic.log" 2>&1; then
  tail -n 40 "${log_dir}/semantic.log" >&2
  exit 1
fi
if rg -n '^(FATAL|ERROR):|Traceback \(most recent call last\):' "${log_dir}/semantic.log"; then
  echo "FAIL semantic: simulator failure in ${log_dir}/semantic.log" >&2
  exit 1
fi
echo "PASS semantic: ${log_dir}/semantic.log"
python3 - "$log_dir" "${gates[@]}" <<'PY'
import json
import hashlib
import os
from datetime import datetime, timezone
from pathlib import Path
import sys
path = Path(sys.argv[1]) / 'gsim_reproduction_summary.json'
path.write_text(json.dumps({'status': 'pass', 'gates': sys.argv[2:] + ['semantic'],
                           'compiler_sha256': hashlib.sha256(Path(os.environ['PYCC']).read_bytes()).hexdigest(),
                           'completed_utc': datetime.now(timezone.utc).isoformat(),
                           'scope': 'GSIM mechanisms adapted to the PYC contract'},
                          indent=2) + '\n')
PY
echo "NewCircuit GSIM mechanism gates passed: ${log_dir}"
