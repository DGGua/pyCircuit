#!/usr/bin/env bash
# Gate: always-on C++ member placement, comb reorder-safety, and manifest fields.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
PYCC="${PYCC:-${ROOT}/.pycircuit_out/toolchain/build/bin/pycc}"
PYC_OPT="${PYC_OPT:-${ROOT}/.pycircuit_out/toolchain/build/bin/pyc-opt}"
EXAMPLE="${ROOT}/designs/examples/counter/counter.py"
INVALID_COMB="${ROOT}/compiler/mlir/test/invalid_comb_effect.mlir"
OUT="${ROOT}/.pycircuit_out/gates/cpp_member_placement_smoke"

if [[ ! -x "${PYCC}" ]]; then
  echo "skip: pycc not built at ${PYCC}" >&2
  exit 0
fi

if "${PYCC}" --help 2>&1 | grep -q -- '--cpp-localize-members'; then
  echo "fail: --cpp-localize-members must stay removed (placement is always-on)" >&2
  exit 1
fi
if ! "${PYCC}" --help 2>&1 | grep -q -- '--cpp-compile-budget'; then
  echo "fail: pycc missing --cpp-compile-budget (Davinci passes =false on hierarchical cores)" >&2
  exit 1
fi

if [[ -x "${PYC_OPT}" ]] && ! "${PYC_OPT}" --help 2>&1 | grep -q 'pyc-cpp-placement'; then
  echo "fail: pyc-opt missing pyc-cpp-placement pass" >&2
  exit 1
fi

if [[ ! -f "${EXAMPLE}" ]]; then
  echo "skip: example not found: ${EXAMPLE}" >&2
  exit 0
fi

rm -rf "${OUT}"
mkdir -p "${OUT}"

if "${PYCC}" "${INVALID_COMB}" --emit=cpp -o "${OUT}/invalid.cpp" \
    >"${OUT}/invalid.stdout" 2>"${OUT}/invalid.stderr"; then
  echo "fail: side-effecting pyc.comb unexpectedly passed verification" >&2
  exit 1
fi
if ! grep -q 'must be memory-effect-free' "${OUT}/invalid.stderr"; then
  echo "fail: missing pyc.comb reorder-safety diagnostic" >&2
  cat "${OUT}/invalid.stderr" >&2
  exit 1
fi

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
  --cpp-shard-max-ast-nodes=2 \
  --build-profile=dev-fast \
  >/dev/null

mkdir -p "${OUT}/cpp_repeat"
"${PYCC}" "${OUT}/counter.pyc" \
  --emit=cpp \
  --out-dir "${OUT}/cpp_repeat" \
  --cpp-split=module \
  --cpp-shard-max-ast-nodes=2 \
  --build-profile=dev-fast \
  >/dev/null

mkdir -p "${OUT}/cpp_budget_off"
"${PYCC}" "${OUT}/counter.pyc" \
  --emit=cpp \
  --out-dir "${OUT}/cpp_budget_off" \
  --cpp-split=module \
  --cpp-shard-max-ast-nodes=2 \
  --build-profile=dev-fast \
  --cpp-compile-budget=false \
  >/dev/null

if [[ ! -f "${OUT}/cpp/counter.hpp" ]]; then
  echo "fail: missing ${OUT}/cpp/counter.hpp" >&2
  exit 1
fi
if [[ ! -f "${OUT}/cpp_budget_off/counter.hpp" ]]; then
  echo "fail: --cpp-compile-budget=false did not emit C++" >&2
  exit 1
fi
if ! cmp -s "${OUT}/cpp/counter.hpp" "${OUT}/cpp_repeat/counter.hpp"; then
  echo "fail: locality scheduling is not deterministic" >&2
  exit 1
fi

manifest="${OUT}/cpp/cpp_compile_manifest.json"
if [[ ! -f "${manifest}" ]]; then
  echo "fail: missing manifest ${manifest}" >&2
  exit 1
fi

python3 - <<'PY' "${manifest}" "${OUT}/cpp/counter.hpp"
import json
import sys
from pathlib import Path

path = Path(sys.argv[1])
hpp = Path(sys.argv[2]).read_text(encoding="utf-8")
data = json.loads(path.read_text(encoding="utf-8"))
profile = data.get("profile_summary") or {}
placement = profile.get("cpp_placement")
if not isinstance(placement, dict):
    raise SystemExit(f"fail: profile_summary.cpp_placement missing in {path}")

for field in (
    "struct_members",
    "local_in_method",
    "probe_pinned_struct",
    "cross_part_promoted",
    "scheduled_cross_method",
    "scheduled_cut_weight",
):
    if field not in placement:
        raise SystemExit(f"fail: manifest missing cpp_placement.{field}")

if placement["local_in_method"] <= 0:
    raise SystemExit("fail: expected local_in_method > 0 on counter")
if "eval_comb_" not in hpp:
    raise SystemExit("fail: generated hpp missing eval_comb_ helper")
print("ok: cpp_placement present; local_in_method=", placement["local_in_method"])
PY

echo "ok: cpp member placement smoke passed"
