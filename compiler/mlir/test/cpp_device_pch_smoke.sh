#!/usr/bin/env bash
# Gate: --cpp-pch flag, manifest precompile_headers, and profile_summary.cpp_pch.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
PYCC="${PYCC:-${ROOT}/.pycircuit_out/toolchain/build/bin/pycc}"
EXAMPLE="${ROOT}/designs/examples/counter/counter.py"
OUT="${ROOT}/.pycircuit_out/gates/cpp_device_pch_smoke"

if [[ ! -x "${PYCC}" ]]; then
  echo "skip: pycc not built at ${PYCC}" >&2
  exit 0
fi

if ! "${PYCC}" --help 2>&1 | grep -q 'cpp-pch'; then
  echo "fail: pycc missing --cpp-pch flag" >&2
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
  --cpp-pch \
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
profile = data.get("profile_summary") or {}
if not profile.get("cpp_pch"):
    raise SystemExit(f"fail: profile_summary.cpp_pch not true in {path}")

pch = data.get("precompile_headers") or []
if not pch:
    raise SystemExit(f"fail: precompile_headers empty in {path}")
if data.get("precompile_headers_mode") != "device_hpp":
    raise SystemExit(f"fail: precompile_headers_mode != device_hpp in {path}")

print("ok: cpp_pch and precompile_headers present in manifest")
PY

echo "ok: cpp device pch smoke passed"
