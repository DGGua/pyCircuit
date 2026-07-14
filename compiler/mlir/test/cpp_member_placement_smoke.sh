#!/usr/bin/env bash
# Gate: C++ member placement pass registration, pipeline wiring, and manifest field.
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

if ! "${PYCC}" --help 2>&1 | rg -q 'cpp-localize-members'; then
  echo "fail: pycc missing --cpp-localize-members flag" >&2
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
if ! rg -q 'must be memory-effect-free' "${OUT}/invalid.stderr"; then
  echo "fail: missing pyc.comb reorder-safety diagnostic" >&2
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
  --cpp-localize-members \
  --cpp-shard-max-ast-nodes=2 \
  --build-profile=dev-fast \
  >/dev/null

mkdir -p "${OUT}/cpp_repeat"
"${PYCC}" "${OUT}/counter.pyc" \
  --emit=cpp \
  --out-dir "${OUT}/cpp_repeat" \
  --cpp-split=module \
  --cpp-localize-members \
  --cpp-shard-max-ast-nodes=2 \
  --build-profile=dev-fast \
  >/dev/null
if ! cmp -s "${OUT}/cpp/counter.cpp" "${OUT}/cpp_repeat/counter.cpp"; then
  echo "fail: locality scheduling is not deterministic" >&2
  exit 1
fi

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

for field in (
    "struct_members",
    "local_in_method",
    "promoted_cross_method",
    "probe_pinned_struct",
    "fixed_order_cross_method",
    "scheduled_cross_method",
    "scheduled_cut_weight",
):
    if field not in placement:
        raise SystemExit(f"fail: manifest missing cpp_placement.{field}")
if placement["scheduled_cross_method"] > placement["fixed_order_cross_method"]:
    raise SystemExit(
        "fail: locality schedule promoted more values than fixed-order chunking"
    )

print("ok: cpp_placement present in manifest")
PY

python3 - <<'PY' "${OUT}/cpp/counter.cpp"
import re
import sys

text = open(sys.argv[1], encoding="utf-8").read()
wrappers = re.findall(
    r"void counter::eval_comb_(\d+)\(\) \{(.*?)\n  \}",
    text,
    flags=re.S,
)
part_calls = 0
for wrapper_index, body in wrappers:
    calls = re.findall(r"eval_comb_(\d+)_part_\d+\(\);", body)
    part_calls += len(calls)
    if any(call_index != wrapper_index for call_index in calls):
        raise SystemExit(
            f"fail: placement/emitter comb index mismatch in eval_comb_{wrapper_index}"
        )
if part_calls == 0:
    raise SystemExit("fail: small chunk budget did not exercise comb helper partitioning")
print("ok: placement/emitter comb helper ownership aligned")
PY

if command -v "${CXX:-c++}" >/dev/null 2>&1; then
  "${CXX:-c++}" -std=c++17 \
    -I"${OUT}/cpp" \
    -I"${ROOT}/.pycircuit_out/toolchain/install/include" \
    -c "${OUT}/cpp/counter.cpp" \
    -o "${OUT}/counter.o"
fi

echo "ok: cpp member placement smoke passed"
