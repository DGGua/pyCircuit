#!/usr/bin/env bash
# Gate the declaration-only callee contract used by incremental multi-.pyc
# builds.  Cycle and depth behavior must be identical to a full-design build;
# no conservative all-ports fallback and no declaration-as-cut fallback is
# permitted.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
PYCC="${PYCC:-${ROOT}/.pycircuit_out/toolchain/build/bin/pycc}"
INPUTS="${ROOT}/compiler/mlir/test/Inputs"
OUT="${PYC_COMB_SUMMARY_GATE_OUT:-${ROOT}/.pycircuit_out/gates/comb_dep_summary}"

if [[ ! -x "${PYCC}" ]]; then
  echo "skip: pycc not built at ${PYCC}" >&2
  exit 0
fi

gate_root="${ROOT}/.pycircuit_out/gates"
mkdir -p "${gate_root}"
OUT=$(python3 -c '
import pathlib, sys
out = pathlib.Path(sys.argv[1]).resolve()
root = pathlib.Path(sys.argv[2]).resolve()
if out == root or root not in out.parents:
    raise SystemExit(f"unsafe gate output path (must be a child of {root}): {out}")
print(out)
' "${OUT}" "${gate_root}")
rm -rf "${OUT}"
mkdir -p "${OUT}"

common_flags=(
  --emit=none
  -o /dev/null
  --comb-partition=static
  --comb-partition-max-nodes=3
  --inline-policy=off
  --hierarchy-policy=strict
)
summary="${OUT}/comb_dep_summary.json"

"${PYCC}" "${INPUTS}/comb_dep_summary_source.mlir" \
  "${common_flags[@]}" --logic-depth=64 --comb-summary-out "${summary}"

python3 - "${summary}" <<'PY'
import json
import pathlib
import sys

data = json.loads(pathlib.Path(sys.argv[1]).read_text(encoding="utf-8"))
assert data["schema"] == "pyc.comb_dep_summary"
assert data["version"] == 1
functions = data["functions"]
assert functions["id"]["results"] == [
    {"arg_deps": [0], "arg_depth": [0], "base_depth": -1}
]
assert functions["const_out"]["results"] == [
    {"arg_deps": [], "arg_depth": [-1], "base_depth": 0}
]
assert functions["chain4"]["results"] == [
    {"arg_deps": [0], "arg_depth": [4], "base_depth": 4}
]
for symbol, entry in functions.items():
    assert entry["version"] == 1
    assert entry["symbol"] == symbol
    assert len(entry["arg_types"]) == entry["num_args"]
    assert len(entry["result_types"]) == entry["num_results"]
PY

# A declaration without hardened metadata is an error, even when a
# conservative dependency approximation might otherwise compile.
if "${PYCC}" "${INPUTS}/comb_dep_summary_split_const.mlir" \
    "${common_flags[@]}" --logic-depth=64 \
    >"${OUT}/missing.stdout" 2>"${OUT}/missing.stderr"; then
  echo "fail: declaration-only callee compiled without a hardened summary" >&2
  exit 1
fi
grep -q "require hardened 'pyc.comb_dep_summary.v1' metadata" \
  "${OUT}/missing.stderr"

# const_out does not depend on its input.  The wire/instance feedback shape is
# therefore acyclic and must not be rejected by an all-input conservative edge.
"${PYCC}" "${INPUTS}/comb_dep_summary_split_const.mlir" \
  "${common_flags[@]}" --logic-depth=64 --comb-summary-in "${summary}"

# id does depend on its input, so the identical split shape is a real cycle.
if "${PYCC}" "${INPUTS}/comb_dep_summary_split_cycle.mlir" \
    "${common_flags[@]}" --logic-depth=64 --comb-summary-in "${summary}" \
    >"${OUT}/cycle.stdout" 2>"${OUT}/cycle.stderr"; then
  echo "fail: exact declaration summary hid a cross-instance cycle" >&2
  exit 1
fi
grep -q "combinational cycle detected" "${OUT}/cycle.stderr"
grep -q "instance u_id @id input#0 -> result#0" "${OUT}/cycle.stderr"

# Two chain4 declaration-only instances contribute eight levels, exactly as
# the full-body graph does: limit 6 rejects, limit 8 accepts.
if "${PYCC}" "${INPUTS}/comb_dep_summary_split_depth.mlir" \
    "${common_flags[@]}" --logic-depth=6 --comb-summary-in "${summary}" \
    >"${OUT}/depth6.stdout" 2>"${OUT}/depth6.stderr"; then
  echo "fail: declaration summary under-counted cross-instance logic depth" >&2
  exit 1
fi
grep -q "logic depth exceeds limit: depth=8 limit=6" "${OUT}/depth6.stderr"
"${PYCC}" "${INPUTS}/comb_dep_summary_split_depth.mlir" \
  "${common_flags[@]}" --logic-depth=8 --comb-summary-in "${summary}"

# The parser must reject internally inconsistent metadata before graph use.
python3 - "${summary}" "${OUT}/malformed.json" <<'PY'
import json
import pathlib
import sys

data = json.loads(pathlib.Path(sys.argv[1]).read_text(encoding="utf-8"))
data["functions"]["id"]["results"][0]["arg_deps"] = []
pathlib.Path(sys.argv[2]).write_text(
    json.dumps(data, sort_keys=True, indent=2) + "\n", encoding="utf-8"
)
PY
if "${PYCC}" "${INPUTS}/comb_dep_summary_split_cycle.mlir" \
    "${common_flags[@]}" --logic-depth=64 \
    --comb-summary-in "${OUT}/malformed.json" \
    >"${OUT}/malformed.stdout" 2>"${OUT}/malformed.stderr"; then
  echo "fail: inconsistent hardened dependency metadata was accepted" >&2
  exit 1
fi
grep -q "arg_deps/arg_depth disagreement" "${OUT}/malformed.stderr"

python3 - "${summary}" "${OUT}/wrong_type.json" <<'PY'
import json
import pathlib
import sys

data = json.loads(pathlib.Path(sys.argv[1]).read_text(encoding="utf-8"))
data["functions"]["id"]["arg_types"] = ["i8"]
pathlib.Path(sys.argv[2]).write_text(
    json.dumps(data, sort_keys=True, indent=2) + "\n", encoding="utf-8"
)
PY
if "${PYCC}" "${INPUTS}/comb_dep_summary_split_cycle.mlir" \
    "${common_flags[@]}" --logic-depth=64 \
    --comb-summary-in "${OUT}/wrong_type.json" \
    >"${OUT}/wrong_type.stdout" 2>"${OUT}/wrong_type.stderr"; then
  echo "fail: wrong declaration summary type signature was accepted" >&2
  exit 1
fi
grep -q "declaration comb summary type mismatch" "${OUT}/wrong_type.stderr"

# Exercise the actual frontend multi-.pyc wiring when a staged runtime is
# available: the full design emits the one shared artifact, and each module
# compilation consumes it before the semantic MLIR gates run.
toolchain_root="${PYC_TOOLCHAIN_ROOT:-${ROOT}/.pycircuit_out/toolchain/install}"
runtime_lib="${toolchain_root}/lib/libpyc4_runtime.a"
if [[ -f "${runtime_lib}" ]]; then
  frontend_out="${OUT}/frontend"
  PYTHONPATH="${ROOT}/compiler/frontend:${PYTHONPATH:-}" \
  PYTHONDONTWRITEBYTECODE=1 \
  PYCC="${PYCC}" \
  PYC_TOOLCHAIN_ROOT="${toolchain_root}" \
    python3 -m pycircuit.cli build \
      "${INPUTS}/comb_dep_summary_frontend.py" \
      --out-dir "${frontend_out}" \
      --target cpp --jobs 2 --profile dev --logic-depth 64 \
      --comb-update dirty --comb-partition static \
      --comb-partition-max-nodes 3 \
      >"${OUT}/frontend_build.stdout"
  "${frontend_out}/cpp_build/build/pyc_tb" \
    >"${OUT}/frontend_run.stdout" 2>&1
  grep -q '^OK$' "${OUT}/frontend_run.stdout"
  python3 - "${frontend_out}" <<'PY'
import hashlib
import json
import pathlib
import sys

out = pathlib.Path(sys.argv[1])
manifest = json.loads((out / "project_manifest.json").read_text(encoding="utf-8"))
cache = json.loads((out / ".build_cache.json").read_text(encoding="utf-8"))
summary_path = out / manifest["comb_dep_summary"]
summary_bytes = summary_path.read_bytes()
summary_hash = hashlib.sha256(summary_bytes).hexdigest()
summary = json.loads(summary_bytes)
assert manifest["comb_dep_summary_sha256"] == summary_hash
assert cache["comb_dep_summary_hash"] == summary_hash
assert summary["schema"] == "pyc.comb_dep_summary"
module_names = sorted(entry["name"] for entry in manifest["modules"])
assert sorted(summary["functions"]) == module_names
top = manifest["top"]
children = [name for name in module_names if name != top]
assert len(children) == 1
caller = out / f"device/modules/{top}.pyc"
caller_text = caller.read_text(encoding="utf-8")
assert f"func.func private @{children[0]}" in caller_text
assert "pyc.instance" in caller_text
PY
else
  echo "skip: frontend split-summary compile requires staged runtime ${runtime_lib}" >&2
fi

echo "ok: hardened split CombDep summaries preserve exact cycle/depth semantics"
