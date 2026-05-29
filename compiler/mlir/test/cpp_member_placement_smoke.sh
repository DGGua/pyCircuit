#!/usr/bin/env bash
# Gate: C++ member placement pass registration, pipeline wiring, and manifest field.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
PYCC="${PYCC:-${ROOT}/.pycircuit_out/toolchain/build/bin/pycc}"
PYC_OPT="${PYC_OPT:-${ROOT}/.pycircuit_out/toolchain/build/bin/pyc-opt}"
EXAMPLE="${ROOT}/designs/examples/counter/counter.py"
OUT="${ROOT}/.pycircuit_out/gates/cpp_member_placement_smoke"

if [[ ! -x "${PYCC}" ]]; then
  echo "skip: pycc not built at ${PYCC}" >&2
  exit 0
fi

if ! "${PYCC}" --help 2>&1 | grep -q 'cpp-localize-members'; then
  echo "fail: pycc missing --cpp-localize-members flag" >&2
  exit 1
fi

if [[ ! -f "${EXAMPLE}" ]]; then
  echo "skip: example not found: ${EXAMPLE}" >&2
  exit 0
fi

rm -rf "${OUT}"
mkdir -p "${OUT}"

export PYTHONPATH="${ROOT}/compiler/frontend:${PYTHONPATH:-}"
python3 - <<'PY' "${EXAMPLE}" "${OUT}/counter.pyc"
import importlib.util
import sys
from pathlib import Path

example, out = sys.argv[1], sys.argv[2]
spec = importlib.util.spec_from_file_location("pyc_smoke_example", example)
mod = importlib.util.module_from_spec(spec)
assert spec.loader is not None
spec.loader.exec_module(mod)
from pycircuit import compile_cycle_aware

circuit = compile_cycle_aware(
    mod.build, name="counter", eager=True, width=8, hierarchical=True
)
mlir = circuit._v5_design.emit_module_mlir_map()["counter"]
Path(out).write_text(mlir, encoding="utf-8")
PY

"${PYCC}" "${OUT}/counter.pyc" \
  --emit=cpp \
  --out-dir "${OUT}/cpp" \
  --cpp-split=module \
  --cpp-localize-members \
  --build-profile=dev-fast \
  >/dev/null

manifest="${OUT}/cpp/cpp_compile_manifest.json"
if [[ ! -f "${manifest}" ]]; then
  echo "fail: missing manifest ${manifest}" >&2
  exit 1
fi

python3 - <<'PY' "${manifest}"
import json
import sys

path = sys.argv[1]
data = json.load(open(path, encoding="utf-8"))
profile = data.get("profile_summary") or data.get("profile") or {}
placement = profile.get("cpp_placement") if isinstance(profile, dict) else None
if placement is None:
    raise SystemExit(f"fail: profile_summary.cpp_placement missing in {path}")

for field in ("struct_members", "local_in_method", "promoted_cross_method", "probe_pinned_struct"):
    if field not in placement:
        raise SystemExit(f"fail: manifest missing cpp_placement.{field}")

print("ok: cpp_placement present in manifest")
PY

echo "ok: cpp member placement smoke passed"
