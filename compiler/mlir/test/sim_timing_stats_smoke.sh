#!/usr/bin/env bash
# Smoke: generated sim stats stay off by default, and JSONL distinguishes a
# full-topo eval from an instance-level fallback eval.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
PYCC="${PYCC:-${ROOT}/.pycircuit_out/toolchain/install/bin/pycc}"
if [[ ! -x "${PYCC}" ]]; then
  PYCC="${ROOT}/.pycircuit_out/toolchain/build/bin/pycc"
fi
CXX="${CXX:-c++}"
OUT="${ROOT}/.pycircuit_out/gates/sim_timing_stats_smoke"
ANALYZER="${ROOT}/flows/tools/perf/analyze_module_fallback_stats.py"

if [[ ! -x "${PYCC}" ]]; then
  echo "skip: pycc not built at ${PYCC}" >&2
  exit 0
fi

rm -rf "${OUT}"
mkdir -p "${OUT}"
export PYTHONPATH="${ROOT}/compiler/frontend:${PYTHONPATH:-}"

python3 - <<'PY' "${OUT}"
import sys
from pathlib import Path
from pycircuit import cas, compile_cycle_aware, wire_of

out = Path(sys.argv[1])

def topo_build(m, domain):
    x = cas(domain, m.input("in", width=8), cycle=0)
    m.output("out", wire_of(x) + 1)

def passthrough(m, domain, inputs=None, prefix="pass"):
    _in = inputs or {}
    x = _in["x"] if "x" in _in else cas(domain, m.input(f"{prefix}_x", width=8), cycle=0)
    y = _in["y"] if "y" in _in else cas(domain, m.input(f"{prefix}_y", width=8), cycle=0)
    m.output(f"{prefix}_ox", wire_of(x))
    m.output(f"{prefix}_oy", wire_of(y))
    return {"ox": x, "oy": y}

def fallback_build(m, domain):
    src = cas(domain, m.input("in", width=8), cycle=0)
    feed = m.new_wire(width=8)
    a = domain.call(
        passthrough,
        inputs={"x": src, "y": cas(domain, feed, cycle=0)},
        prefix="a",
    )
    b = domain.call(passthrough, inputs={"x": a["ox"], "y": a["oy"]}, prefix="b")
    m.assign(feed, wire_of(b["ox"]))
    m.output("out", wire_of(b["oy"]))

topo = compile_cycle_aware(topo_build, name="topo_top", eager=True, hierarchical=True)
fallback = compile_cycle_aware(fallback_build, name="fb_top", eager=True, hierarchical=True)
(out / "topo.pyc").write_text(topo._v5_design.emit_mlir(), encoding="utf-8")
(out / "fallback.pyc").write_text(fallback._v5_design.emit_mlir(), encoding="utf-8")
PY

emit_and_compile() {
  local name="$1"
  local expect="$2"
  local struct_name="$3"
  local src="${OUT}/${name}.pyc"
  local cpp="${OUT}/${name}.cpp"
  local bin="${OUT}/${name}.bin"
  "${PYCC}" "${src}" --emit=cpp -o "${cpp}"
  if ! grep -q '#include <chrono>' "${cpp}"; then
    echo "fail: ${cpp} missing chrono include" >&2
    exit 1
  fi
  cat >"${OUT}/${name}_main.cpp" <<EOF
#include <cstdint>
#include <iostream>

#include "${name}.cpp"

int main() {
  pyc::gen::${struct_name} dut;
  dut.in = pyc::cpp::Wire<8>(41);
  dut.eval();
  std::uint64_t first = dut.out.value();
  dut.eval();
  std::uint64_t second = dut.out.value();
  if (first != ${expect} || second != ${expect}) {
    std::cerr << "${name} mismatch: first=" << first << " second=" << second
              << " expect=${expect}\n";
    return 1;
  }
  std::cout << "${name} " << first << " " << second << "\n";
  return 0;
}
EOF
  "${CXX}" -std=c++17 -O2 -I"${ROOT}/.pycircuit_out/toolchain/install/include" \
    -o "${bin}" "${OUT}/${name}_main.cpp"
}

# topo_top: out = in + 1. fb_top: out converges to in.
emit_and_compile topo 42 topo_top
emit_and_compile fallback 41 fb_top

run_pair() {
  local name="$1"
  local off="${OUT}/${name}.off.txt"
  local on="${OUT}/${name}.on.txt"
  local stats="${OUT}/${name}.jsonl"
  env -u PYC_SIM_STATS -u PYC_SIM_TIMING -u PYC_SIM_STATS_PATH \
    "${OUT}/${name}.bin" >"${off}"
  if [[ -e "${stats}" ]]; then
    echo "fail: stats file created while stats path was unset" >&2
    exit 1
  fi
  PYC_SIM_STATS=1 PYC_SIM_TIMING=1 PYC_SIM_STATS_PATH="${stats}" \
    "${OUT}/${name}.bin" >"${on}"
  if ! cmp -s "${off}" "${on}"; then
    echo "fail: ${name} functional output changed with stats enabled" >&2
    diff -u "${off}" "${on}" >&2 || true
    exit 1
  fi
  if [[ ! -s "${stats}" ]]; then
    echo "fail: ${name} did not write ${stats}" >&2
    exit 1
  fi
}

run_pair topo
run_pair fallback

python3 - <<'PY' "${OUT}/topo.jsonl" "${OUT}/fallback.jsonl"
import json
import sys

def rows(path):
    return [json.loads(line) for line in open(path, encoding="utf-8") if line.strip()]

topo = rows(sys.argv[1])
fb = rows(sys.argv[2])
top = topo[0]
if top["topo_eval_calls"] <= 0 or top["fallback_calls"] != 0 or top["eval_total_ns"] <= 0:
    raise SystemExit(f"topo stats unexpected: {top}")
if fb[0]["fallback_calls"] <= 0 or fb[0]["fallback_total_ns"] <= 0 or fb[0]["eval_total_ns"] <= 0:
    raise SystemExit(f"fallback top unexpected: {fb[0]}")
if fb[0]["fallback_iterations"] <= 0:
    raise SystemExit(f"fallback iterations unexpected: {fb[0]}")
hist = (
    fb[0]["fallback_iter_hist_0"]
    + fb[0]["fallback_iter_hist_1"]
    + fb[0]["fallback_iter_hist_2"]
    + fb[0]["fallback_iter_hist_3"]
    + fb[0]["fallback_iter_hist_4p"]
)
if hist != fb[0]["fallback_calls"]:
    raise SystemExit(f"histogram {hist} != fallback_calls {fb[0]['fallback_calls']}")
if len(fb) < 3:
    raise SystemExit(f"expected top plus two instances, got {len(fb)} records")
print("smoke jsonl ok")
PY

python3 "${ANALYZER}" --require-timing \
  --out-json "${OUT}/one.json" --out-md "${OUT}/one.md" \
  "${OUT}/fallback.jsonl"
for i in 1 2 3 4 5; do
  cp "${OUT}/fallback.jsonl" "${OUT}/fallback-${i}.jsonl"
done
python3 "${ANALYZER}" --require-timing \
  --out-json "${OUT}/five.json" --out-md "${OUT}/five.md" \
  "${OUT}/fallback-1.jsonl" "${OUT}/fallback-2.jsonl" "${OUT}/fallback-3.jsonl" \
  "${OUT}/fallback-4.jsonl" "${OUT}/fallback-5.jsonl"
python3 - <<'PY' "${OUT}/five.json"
import json
import sys
payload = json.load(open(sys.argv[1], encoding="utf-8"))
agg = payload["aggregate"]
if agg["runs"] != 5:
    raise SystemExit(agg)
if not (agg["fallback_share_min"] <= agg["fallback_share_median"] <= agg["fallback_share_max"]):
    raise SystemExit(agg)
print("parser aggregate ok", agg["fallback_share_median"])
PY

echo "ok: sim timing stats smoke"
