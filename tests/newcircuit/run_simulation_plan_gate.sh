#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)"
source "${repo_root}/flows/scripts/lib.sh"
if [[ -z "${PYCC:-}" && -x "${repo_root}/.pycircuit_out/toolchain/build/bin/pycc" ]]; then
  PYCC="${repo_root}/.pycircuit_out/toolchain/build/bin/pycc"
fi
pyc_find_pycc
if [[ ! -d "${PYC_TOOLCHAIN_ROOT}/include/cpp" ]]; then
  staged_root="${repo_root}/.pycircuit_out/toolchain/install"
  [[ -d "${staged_root}/include/cpp" ]] || pyc_die "missing staged C++ runtime headers"
  PYC_TOOLCHAIN_ROOT="${staged_root}"
fi

gate_dir="$(mktemp -d)"
trap 'rm -rf -- "${gate_dir}"' EXIT

PYTHONPATH="$(pyc_pythonpath)" PYTHONDONTWRITEBYTECODE=1 \
  PYCC="${PYCC}" PYC_TOOLCHAIN_ROOT="${PYC_TOOLCHAIN_ROOT}" \
  python3 -m pycircuit.cli build \
    "${repo_root}/designs/examples/wire_ops/tb_wire_ops.py" \
    --out-dir "${gate_dir}/default" --target cpp --jobs 2 --logic-depth 256

cpp_executable="$(python3 - "${gate_dir}/default/project_manifest.json" <<'PY'
import json
import sys
with open(sys.argv[1], encoding="utf-8") as manifest:
    print(json.load(manifest)["cpp_executable"])
PY
)"
"${cpp_executable}"

if ! rg -q 'eval_sim_group_' "${gate_dir}/default/device/cpp"; then
  echo 'C++ default path did not use a planned SimGraph group' >&2
  exit 1
fi

design="${gate_dir}/default/device/design.pyc"
"${PYCC}" "${design}" --emit=cpp --sim-mode=cpp-only \
  --cpp-only-preserve-ops --logic-depth=256 -o "${gate_dir}/preserve.cpp"
if rg -q 'eval_sim_group_' "${gate_dir}/preserve.cpp"; then
  echo 'preserve-ops mode unexpectedly grouped operations' >&2
  exit 1
fi

"${PYCC}" "${design}" --emit=cpp --emit-structural=on \
  --logic-depth=256 -o "${gate_dir}/structural.cpp"
if rg -q 'eval_sim_group_' "${gate_dir}/structural.cpp"; then
  echo 'structural mode unexpectedly grouped operations' >&2
  exit 1
fi

"${PYCC}" "${design}" --emit=cpp --cpp-split=none \
  --logic-depth=256 --out-dir="${gate_dir}/nosplit"
test -s "${gate_dir}/nosplit/wire_ops.hpp"

# Existing pyc.comb input must retain its nested AST operations and execute
# through the graph and plan stages.
"${PYCC}" "${repo_root}/tests/newcircuit/nested_comb.pyc" \
  --emit=cpp -o "${gate_dir}/nested_comb.cpp"
rg -q 'eval_comb_0' "${gate_dir}/nested_comb.cpp"
cat > "${gate_dir}/nested_comb_harness.cpp" <<'CPP'
#include "nested_comb.cpp"
int main() {
  pyc::gen::top sim;
  sim.a = pyc::cpp::Wire<8>(2);
  sim.b = pyc::cpp::Wire<8>(3);
  sim.comb();
  return sim.y.value() == 5 ? 0 : 1;
}
CPP
"${CXX:-c++}" -std=c++17 -I "${PYC_TOOLCHAIN_ROOT}/include" -I "${gate_dir}" \
  "${gate_dir}/nested_comb_harness.cpp" -o "${gate_dir}/nested_comb_harness"
"${gate_dir}/nested_comb_harness"

# Nested comb regions keep pure steps and yields in the graph. An effectful
# step must fail at the dialect verifier before graph construction.
"${PYCC}" "${repo_root}/tests/newcircuit/double_nested_comb.pyc" \
  --emit=cpp -o "${gate_dir}/double_nested_comb.cpp"
rg -q 'pyc_comb_2 = pyc_add_1' "${gate_dir}/double_nested_comb.cpp" -F
cat > "${gate_dir}/double_nested_comb_harness.cpp" <<'CPP'
#include "double_nested_comb.cpp"
int main() {
  pyc::gen::top sim;
  sim.a = pyc::cpp::Wire<8>(2);
  sim.b = pyc::cpp::Wire<8>(3);
  sim.eval();
  if (sim.y.value() != 7) return 1;
  sim.a = pyc::cpp::Wire<8>(0xfe);
  sim.b = pyc::cpp::Wire<8>(5);
  sim.eval();
  return sim.y.value() == 0xfd ? 0 : 2;
}
CPP
"${CXX:-c++}" -std=c++17 -I "${PYC_TOOLCHAIN_ROOT}/include" -I "${gate_dir}" \
  "${gate_dir}/double_nested_comb_harness.cpp" -o "${gate_dir}/double_nested_comb_harness"
"${gate_dir}/double_nested_comb_harness"
"${PYCC}" "${repo_root}/tests/newcircuit/double_nested_comb.pyc" \
  --emit=cpp --cpp-shard-max-ast-nodes=1 \
  -o "${gate_dir}/double_nested_comb_chunk.cpp"
rg -F -q 'eval_comb_0_part_1()' "${gate_dir}/double_nested_comb_chunk.cpp"
sed 's/"double_nested_comb.cpp"/"double_nested_comb_chunk.cpp"/' \
  "${gate_dir}/double_nested_comb_harness.cpp" \
  > "${gate_dir}/double_nested_comb_chunk_harness.cpp"
"${CXX:-c++}" -std=c++17 -I "${PYC_TOOLCHAIN_ROOT}/include" -I "${gate_dir}" \
  "${gate_dir}/double_nested_comb_chunk_harness.cpp" \
  -o "${gate_dir}/double_nested_comb_chunk_harness"
"${gate_dir}/double_nested_comb_chunk_harness"
"${PYCC}" "${repo_root}/tests/newcircuit/cdc_graph_binding.pyc" \
  --emit=cpp -o "${gate_dir}/cdc_graph_binding.cpp"
rg -F -q 'pyc::cpp::pyc_cdc_sync<8, 3> pyc_cdc_sync_1_inst' \
  "${gate_dir}/cdc_graph_binding.cpp"
cat > "${gate_dir}/cdc_graph_binding_harness.cpp" <<'CPP'
#include "cdc_graph_binding.cpp"
int main() {
  pyc::gen::top sim;
  sim.rst = pyc::cpp::Wire<1>(0);
  sim.d = pyc::cpp::Wire<8>(0x5a);
  for (unsigned edge = 0; edge < 3; ++edge) {
    sim.clk = pyc::cpp::Wire<1>(1);
    sim.tick_compute();
    sim.tick_commit();
    sim.eval();
    if (sim.q.value() != (edge == 2 ? 0x5a : 0)) return edge + 1;
    sim.clk = pyc::cpp::Wire<1>(0);
    sim.tick_compute();
    sim.tick_commit();
  }
  sim.rst = pyc::cpp::Wire<1>(1);
  sim.clk = pyc::cpp::Wire<1>(1);
  sim.tick_compute();
  sim.tick_commit();
  sim.eval();
  return sim.q.value() == 0 ? 0 : 4;
}
CPP
"${CXX:-c++}" -std=c++17 -I "${PYC_TOOLCHAIN_ROOT}/include" -I "${gate_dir}" \
  "${gate_dir}/cdc_graph_binding_harness.cpp" \
  -o "${gate_dir}/cdc_graph_binding_harness"
"${gate_dir}/cdc_graph_binding_harness"
"${PYCC}" "${repo_root}/tests/newcircuit/fifo_graph_binding.pyc" \
  --emit=cpp -o "${gate_dir}/fifo_graph_binding.cpp"
rg -F -q 'pyc::cpp::pyc_fifo<8, 2> pyc_fifo_1_inst' \
  "${gate_dir}/fifo_graph_binding.cpp"
cat > "${gate_dir}/fifo_graph_binding_harness.cpp" <<'CPP'
#include "fifo_graph_binding.cpp"
int main() {
  pyc::gen::top sim;
  sim.clk = pyc::cpp::Wire<1>(0);
  sim.rst = pyc::cpp::Wire<1>(0);
  sim.in_valid = pyc::cpp::Wire<1>(1);
  sim.in_data = pyc::cpp::Wire<8>(0x5a);
  sim.out_ready = pyc::cpp::Wire<1>(0);
  sim.eval();
  if (sim.in_ready.value() != 1 || sim.out_valid.value() != 0) return 1;
  sim.clk = pyc::cpp::Wire<1>(1);
  sim.tick_compute();
  sim.tick_commit();
  sim.eval();
  if (sim.out_valid.value() != 1 || sim.out_data.value() != 0x5a) return 2;
  sim.in_valid = pyc::cpp::Wire<1>(0);
  sim.out_ready = pyc::cpp::Wire<1>(1);
  sim.clk = pyc::cpp::Wire<1>(0);
  sim.tick_compute();
  sim.tick_commit();
  sim.clk = pyc::cpp::Wire<1>(1);
  sim.tick_compute();
  sim.tick_commit();
  sim.eval();
  return sim.out_valid.value() == 0 && sim.in_ready.value() == 1 ? 0 : 3;
}
CPP
"${CXX:-c++}" -std=c++17 -I "${PYC_TOOLCHAIN_ROOT}/include" -I "${gate_dir}" \
  "${gate_dir}/fifo_graph_binding_harness.cpp" \
  -o "${gate_dir}/fifo_graph_binding_harness"
"${gate_dir}/fifo_graph_binding_harness"
"${PYCC}" "${repo_root}/tests/newcircuit/async_fifo_graph_binding.pyc" \
  --emit=cpp -o "${gate_dir}/async_fifo_graph_binding.cpp"
rg -F -q 'pyc::cpp::pyc_async_fifo<8, 4> pyc_async_fifo_1_inst' \
  "${gate_dir}/async_fifo_graph_binding.cpp"
cat > "${gate_dir}/async_fifo_graph_binding_harness.cpp" <<'CPP'
#include "async_fifo_graph_binding.cpp"
int main() {
  pyc::gen::top sim;
  sim.in_clk = pyc::cpp::Wire<1>(0);
  sim.out_clk = pyc::cpp::Wire<1>(0);
  sim.in_rst = pyc::cpp::Wire<1>(0);
  sim.out_rst = pyc::cpp::Wire<1>(0);
  sim.in_valid = pyc::cpp::Wire<1>(1);
  sim.in_data = pyc::cpp::Wire<8>(0x7b);
  sim.out_ready = pyc::cpp::Wire<1>(0);
  sim.eval();
  if (sim.in_ready.value() != 1 || sim.out_valid.value() != 0) return 1;
  sim.in_clk = pyc::cpp::Wire<1>(1);
  sim.tick_compute();
  sim.tick_commit();
  sim.in_valid = pyc::cpp::Wire<1>(0);
  sim.in_clk = pyc::cpp::Wire<1>(0);
  sim.tick_compute();
  sim.tick_commit();
  for (unsigned edge = 0; edge < 3; ++edge) {
    sim.out_clk = pyc::cpp::Wire<1>(1);
    sim.tick_compute();
    sim.tick_commit();
    sim.eval();
    if (sim.out_valid.value() != (edge == 2 ? 1 : 0)) return edge + 2;
    sim.out_clk = pyc::cpp::Wire<1>(0);
    sim.tick_compute();
    sim.tick_commit();
  }
  if (sim.out_data.value() != 0x7b) return 5;
  sim.out_ready = pyc::cpp::Wire<1>(1);
  sim.out_clk = pyc::cpp::Wire<1>(1);
  sim.tick_compute();
  sim.tick_commit();
  sim.eval();
  return sim.out_valid.value() == 0 ? 0 : 6;
}
CPP
"${CXX:-c++}" -std=c++17 -I "${PYC_TOOLCHAIN_ROOT}/include" -I "${gate_dir}" \
  "${gate_dir}/async_fifo_graph_binding_harness.cpp" \
  -o "${gate_dir}/async_fifo_graph_binding_harness"
"${gate_dir}/async_fifo_graph_binding_harness"
"${PYCC}" "${repo_root}/tests/newcircuit/sync_mem_graph_binding.pyc" \
  --emit=cpp -o "${gate_dir}/sync_mem_graph_binding.cpp"
rg -F -q 'pyc::cpp::pyc_sync_mem<2, 8, 4> *memory0' \
  "${gate_dir}/sync_mem_graph_binding.cpp"
cat > "${gate_dir}/sync_mem_graph_binding_harness.cpp" <<'CPP'
#include "sync_mem_graph_binding.cpp"
int main() {
  pyc::gen::top sim;
  sim.clk = pyc::cpp::Wire<1>(0);
  sim.rst = pyc::cpp::Wire<1>(0);
  sim.ren = pyc::cpp::Wire<1>(1);
  sim.raddr = pyc::cpp::Wire<2>(1);
  sim.wvalid = pyc::cpp::Wire<1>(1);
  sim.waddr = pyc::cpp::Wire<2>(1);
  sim.wdata = pyc::cpp::Wire<8>(0xa5);
  sim.wstrb = pyc::cpp::Wire<1>(1);
  sim.clk = pyc::cpp::Wire<1>(1);
  sim.tick_compute();
  sim.tick_commit();
  sim.eval();
  if (sim.rdata.value() != 0) return 1;
  sim.clk = pyc::cpp::Wire<1>(0);
  sim.tick_compute();
  sim.tick_commit();
  sim.wvalid = pyc::cpp::Wire<1>(0);
  sim.clk = pyc::cpp::Wire<1>(1);
  sim.tick_compute();
  sim.tick_commit();
  sim.eval();
  return sim.rdata.value() == 0xa5 ? 0 : 2;
}
CPP
"${CXX:-c++}" -std=c++17 -I "${PYC_TOOLCHAIN_ROOT}/include" -I "${gate_dir}" \
  "${gate_dir}/sync_mem_graph_binding_harness.cpp" \
  -o "${gate_dir}/sync_mem_graph_binding_harness"
"${gate_dir}/sync_mem_graph_binding_harness"
"${PYCC}" "${repo_root}/tests/newcircuit/sync_mem_dp_graph_binding.pyc" \
  --emit=cpp -o "${gate_dir}/sync_mem_dp_graph_binding.cpp"
rg -F -q 'pyc::cpp::pyc_sync_mem_dp<2, 8, 4> *memory_dp0' \
  "${gate_dir}/sync_mem_dp_graph_binding.cpp"
cat > "${gate_dir}/sync_mem_dp_graph_binding_harness.cpp" <<'CPP'
#include "sync_mem_dp_graph_binding.cpp"
int main() {
  pyc::gen::top sim;
  sim.clk = pyc::cpp::Wire<1>(0);
  sim.rst = pyc::cpp::Wire<1>(0);
  sim.ren0 = pyc::cpp::Wire<1>(1);
  sim.raddr0 = pyc::cpp::Wire<2>(1);
  sim.ren1 = pyc::cpp::Wire<1>(1);
  sim.raddr1 = pyc::cpp::Wire<2>(2);
  sim.wvalid = pyc::cpp::Wire<1>(1);
  sim.waddr = pyc::cpp::Wire<2>(1);
  sim.wdata = pyc::cpp::Wire<8>(0xa5);
  sim.wstrb = pyc::cpp::Wire<1>(1);
  sim.clk = pyc::cpp::Wire<1>(1);
  sim.tick_compute();
  sim.tick_commit();
  sim.eval();
  if (sim.rdata0.value() != 0 || sim.rdata1.value() != 0) return 1;
  sim.clk = pyc::cpp::Wire<1>(0);
  sim.tick_compute();
  sim.tick_commit();
  sim.wvalid = pyc::cpp::Wire<1>(0);
  sim.clk = pyc::cpp::Wire<1>(1);
  sim.tick_compute();
  sim.tick_commit();
  sim.eval();
  return sim.rdata0.value() == 0xa5 && sim.rdata1.value() == 0 ? 0 : 2;
}
CPP
"${CXX:-c++}" -std=c++17 -I "${PYC_TOOLCHAIN_ROOT}/include" -I "${gate_dir}" \
  "${gate_dir}/sync_mem_dp_graph_binding_harness.cpp" \
  -o "${gate_dir}/sync_mem_dp_graph_binding_harness"
"${gate_dir}/sync_mem_dp_graph_binding_harness"
"${PYCC}" "${repo_root}/tests/newcircuit/byte_mem_graph_binding.pyc" \
  --emit=cpp -o "${gate_dir}/byte_mem_graph_binding.cpp"
rg -F -q 'pyc::cpp::pyc_byte_mem<2, 8, 4> byte_memory0' \
  "${gate_dir}/byte_mem_graph_binding.cpp"
cat > "${gate_dir}/byte_mem_graph_binding_harness.cpp" <<'CPP'
#include "byte_mem_graph_binding.cpp"
int main() {
  pyc::gen::top sim;
  sim.clk = pyc::cpp::Wire<1>(0);
  sim.rst = pyc::cpp::Wire<1>(0);
  sim.raddr = pyc::cpp::Wire<2>(1);
  sim.wvalid = pyc::cpp::Wire<1>(1);
  sim.waddr = pyc::cpp::Wire<2>(1);
  sim.wdata = pyc::cpp::Wire<8>(0xa5);
  sim.wstrb = pyc::cpp::Wire<1>(1);
  sim.eval();
  if (sim.rdata.value() != 0) return 1;
  sim.clk = pyc::cpp::Wire<1>(1);
  sim.tick_compute();
  sim.tick_commit();
  sim.eval();
  if (sim.rdata.value() != 0xa5) return 2;
  sim.raddr = pyc::cpp::Wire<2>(2);
  sim.eval();
  return sim.rdata.value() == 0 ? 0 : 3;
}
CPP
"${CXX:-c++}" -std=c++17 -I "${PYC_TOOLCHAIN_ROOT}/include" -I "${gate_dir}" \
  "${gate_dir}/byte_mem_graph_binding_harness.cpp" \
  -o "${gate_dir}/byte_mem_graph_binding_harness"
"${gate_dir}/byte_mem_graph_binding_harness"
if command -v iverilog >/dev/null 2>&1; then
  "${PYCC}" "${repo_root}/tests/newcircuit/double_nested_comb.pyc" \
    --emit=verilog -o "${gate_dir}/double_nested_comb.v"
  cat > "${gate_dir}/double_nested_comb_tb.sv" <<'SV'
module double_nested_comb_tb;
  reg [7:0] a, b;
  wire [7:0] y;
  top dut(.a(a), .b(b), .y(y));
  initial begin
    a = 8'd2; b = 8'd3; #1;
    if (y !== 8'd7) $fatal;
    a = 8'hfe; b = 8'd5; #1;
    if (y !== 8'hfd) $fatal;
    $finish;
  end
endmodule
SV
  iverilog -g2012 -s double_nested_comb_tb -I"${repo_root}/runtime/verilog" \
    -o "${gate_dir}/double_nested_comb_tb" "${gate_dir}/double_nested_comb.v" \
    "${gate_dir}/double_nested_comb_tb.sv"
  "${gate_dir}/double_nested_comb_tb"
fi
if "${PYCC}" "${repo_root}/tests/newcircuit/impure_comb.pyc" \
  --emit=cpp -o "${gate_dir}/impure_comb.cpp" \
  >"${gate_dir}/impure_comb.log" 2>&1; then
  echo 'effectful pyc.comb body passed the dialect verifier' >&2
  exit 1
fi
rg -q 'pyc.comb body must contain only pure operations' \
  "${gate_dir}/impure_comb.log" -F

# Dimension insertion is a graph-owned pure expression in both vector layouts.
"${PYCC}" "${repo_root}/tests/newcircuit/v_broadcast_dim.pyc" \
  --emit=cpp -o "${gate_dir}/v_broadcast_dim.cpp"
rg -q 'a[0], a[1], a[0], a[1], a[0], a[1]' \
  "${gate_dir}/v_broadcast_dim.cpp" -F
cat > "${gate_dir}/v_broadcast_dim_harness.cpp" <<'CPP'
#include "v_broadcast_dim.cpp"
int main() {
  pyc::gen::top sim;
  sim.a[0] = pyc::cpp::Wire<8>(3);
  sim.a[1] = pyc::cpp::Wire<8>(7);
  sim.eval();
  for (int i = 0; i < 3; ++i)
    if (sim.rows[i][0].value() != 3 || sim.rows[i][1].value() != 7)
      return 1;
  for (int j = 0; j < 3; ++j)
    if (sim.cols[0][j].value() != 3 || sim.cols[1][j].value() != 7)
      return 2;
  return 0;
}
CPP
"${CXX:-c++}" -std=c++17 -I "${PYC_TOOLCHAIN_ROOT}/include" -I "${gate_dir}" \
  "${gate_dir}/v_broadcast_dim_harness.cpp" -o "${gate_dir}/v_broadcast_dim_harness"
"${gate_dir}/v_broadcast_dim_harness"
if command -v iverilog >/dev/null 2>&1; then
  "${PYCC}" "${repo_root}/tests/newcircuit/v_broadcast_dim.pyc" \
    --emit=verilog -o "${gate_dir}/v_broadcast_dim.v"
  cat > "${gate_dir}/v_broadcast_dim_tb.sv" <<'SV'
module v_broadcast_dim_tb;
  reg [15:0] a;
  wire [47:0] rows, cols;
  top dut(.a(a), .rows(rows), .cols(cols));
  integer i;
  initial begin
    a = 16'h0703; #1;
    for (i = 0; i < 3; i = i + 1)
      if (rows[i*16 +: 8] !== 8'd3 ||
          rows[i*16+8 +: 8] !== 8'd7 ||
          cols[i*8 +: 8] !== 8'd3 ||
          cols[(i+3)*8 +: 8] !== 8'd7) $fatal;
    $finish;
  end
endmodule
SV
  iverilog -g2012 -s v_broadcast_dim_tb -I"${repo_root}/runtime/verilog" \
    -o "${gate_dir}/v_broadcast_dim_tb" "${gate_dir}/v_broadcast_dim.v" \
    "${gate_dir}/v_broadcast_dim_tb.sv"
  "${gate_dir}/v_broadcast_dim_tb"
fi

# Fixed-index reads of a dimension broadcast expose only the selected source
# lane; a broadcast along the inner axis becomes a one-dimensional broadcast.
"${PYCC}" "${repo_root}/tests/newcircuit/v_broadcast_dim_lanes.pyc" \
  --emit=cpp --dump-pass-ir="${gate_dir}/v_broadcast_dim_lanes_ir" \
  --dump-pass-ir-filter=comb-canonicalize \
  -o "${gate_dir}/v_broadcast_dim_lanes.cpp"
! rg -q 'pyc.v_broadcast_dim' \
  "${gate_dir}/v_broadcast_dim_lanes_ir"/*after*.mlir
rg -q 'pyc.v_broadcast ' \
  "${gate_dir}/v_broadcast_dim_lanes_ir"/*after*.mlir
"${CXX:-c++}" -std=c++17 -I "${PYC_TOOLCHAIN_ROOT}/include" \
  -I "${gate_dir}" "${repo_root}/tests/newcircuit/v_broadcast_dim_lanes_harness.cpp" \
  -o "${gate_dir}/v_broadcast_dim_lanes_harness"
"${gate_dir}/v_broadcast_dim_lanes_harness"
if command -v iverilog >/dev/null 2>&1; then
  "${PYCC}" "${repo_root}/tests/newcircuit/v_broadcast_dim_lanes.pyc" \
    --emit=verilog -o "${gate_dir}/v_broadcast_dim_lanes.v"
  iverilog -g2012 -s v_broadcast_dim_lanes_tb \
    -I"${repo_root}/runtime/verilog" -o "${gate_dir}/v_broadcast_dim_lanes_tb" \
    "${gate_dir}/v_broadcast_dim_lanes.v" \
    "${repo_root}/tests/newcircuit/v_broadcast_dim_lanes_tb.sv"
  "${gate_dir}/v_broadcast_dim_lanes_tb"
fi

# When only selected rows or columns of a rank-2 reduction are read, lower
# those lanes before both backends and discard the full aggregate reduction.
"${PYCC}" "${repo_root}/tests/newcircuit/reduce_observed_lanes.pyc" \
  --emit=cpp --dump-pass-ir="${gate_dir}/reduce_observed_lanes_ir" \
  --dump-pass-ir-filter=comb-canonicalize \
  -o "${gate_dir}/reduce_observed_lanes.cpp"
! rg -q 'pyc.v_.*_reduce .*-> vector<' \
  "${gate_dir}/reduce_observed_lanes_ir"/*after*.mlir
"${CXX:-c++}" -std=c++17 -I "${PYC_TOOLCHAIN_ROOT}/include" \
  -I "${gate_dir}" "${repo_root}/tests/newcircuit/reduce_observed_lanes_harness.cpp" \
  -o "${gate_dir}/reduce_observed_lanes_harness"
"${gate_dir}/reduce_observed_lanes_harness"
if command -v iverilog >/dev/null 2>&1; then
  "${PYCC}" "${repo_root}/tests/newcircuit/reduce_observed_lanes.pyc" \
    --emit=verilog -o "${gate_dir}/reduce_observed_lanes.v"
  iverilog -g2012 -s reduce_observed_lanes_tb \
    -I"${repo_root}/runtime/verilog" -o "${gate_dir}/reduce_observed_lanes_tb" \
    "${gate_dir}/reduce_observed_lanes.v" \
    "${repo_root}/tests/newcircuit/reduce_observed_lanes_tb.sv"
  "${gate_dir}/reduce_observed_lanes_tb"
fi

# Selective rank-2 reduction also exposes fixed scalar lanes of vector state.
"${PYCC}" "${repo_root}/tests/newcircuit/reduce_register_lanes.pyc" \
  --emit=cpp -o "${gate_dir}/reduce_register_lanes.cpp"
test "$(rg -c 'pyc::cpp::pyc_reg<8> \*' \
  "${gate_dir}/reduce_register_lanes.cpp")" = 3
"${CXX:-c++}" -std=c++17 -I "${PYC_TOOLCHAIN_ROOT}/include" \
  -I "${gate_dir}" "${repo_root}/tests/newcircuit/reduce_register_lanes_harness.cpp" \
  -o "${gate_dir}/reduce_register_lanes_harness"
"${gate_dir}/reduce_register_lanes_harness"
if command -v iverilog >/dev/null 2>&1; then
  "${PYCC}" "${repo_root}/tests/newcircuit/reduce_register_lanes.pyc" \
    --emit=verilog -o "${gate_dir}/reduce_register_lanes.v"
  iverilog -g2012 -s reduce_register_lanes_tb \
    -I"${repo_root}/runtime/verilog" -o "${gate_dir}/reduce_register_lanes_tb" \
    "${gate_dir}/reduce_register_lanes.v" \
    "${repo_root}/tests/newcircuit/reduce_register_lanes_tb.sv"
  "${gate_dir}/reduce_register_lanes_tb"
fi

# Vector arith.select is elementwise under a scalar condition; fixed output
# readers materialize only their selected scalar lanes.
"${PYCC}" "${repo_root}/tests/newcircuit/select_observed_lanes.pyc" \
  --emit=cpp --dump-pass-ir="${gate_dir}/select_observed_lanes_ir" \
  --dump-pass-ir-filter=comb-canonicalize \
  -o "${gate_dir}/select_observed_lanes.cpp"
! rg -q 'arith.select .*vector<' \
  "${gate_dir}/select_observed_lanes_ir"/*after*.mlir
"${CXX:-c++}" -std=c++17 -I "${PYC_TOOLCHAIN_ROOT}/include" \
  -I "${gate_dir}" "${repo_root}/tests/newcircuit/select_observed_lanes_harness.cpp" \
  -o "${gate_dir}/select_observed_lanes_harness"
"${gate_dir}/select_observed_lanes_harness"
if command -v iverilog >/dev/null 2>&1; then
  "${PYCC}" "${repo_root}/tests/newcircuit/select_observed_lanes.pyc" \
    --emit=verilog -o "${gate_dir}/select_observed_lanes.v"
  iverilog -g2012 -s select_observed_lanes_tb \
    -I"${repo_root}/runtime/verilog" -o "${gate_dir}/select_observed_lanes_tb" \
    "${gate_dir}/select_observed_lanes.v" \
    "${repo_root}/tests/newcircuit/select_observed_lanes_tb.sv"
  "${gate_dir}/select_observed_lanes_tb"
fi

"${PYCC}" "${repo_root}/tests/newcircuit/graph_assert.pyc" \
  --emit=cpp -o "${gate_dir}/graph_assert.cpp"
rg -q 'assert values differ' "${gate_dir}/graph_assert.cpp" -F
cat > "${gate_dir}/graph_assert_harness.cpp" <<'CPP'
#include "graph_assert.cpp"
int main(int argc, char **) {
  pyc::gen::top sim;
  sim.a = pyc::cpp::Wire<8>(2);
  sim.b = pyc::cpp::Wire<8>(argc > 1 ? 3 : 2);
  sim.eval();
  return sim.y.value() == 2 ? 0 : 1;
}
CPP
"${CXX:-c++}" -std=c++17 -I "${PYC_TOOLCHAIN_ROOT}/include" -I "${gate_dir}" \
  "${gate_dir}/graph_assert_harness.cpp" -o "${gate_dir}/graph_assert_harness"
"${gate_dir}/graph_assert_harness"
if (ulimit -c 0; "${gate_dir}/graph_assert_harness" fail) \
  >"${gate_dir}/graph_assert_fail.log" 2>&1; then
  echo 'false graph-owned assertion did not abort' >&2
  exit 1
fi
rg -q 'assert values differ' "${gate_dir}/graph_assert_fail.log" -F

# The graph activation pass must skip a stable pure group and recompute after
# an external input changes. Statistics are generated by the planned group.
"${PYCC}" "${repo_root}/tests/newcircuit/group_activation.pyc" \
  --emit=cpp -o "${gate_dir}/group_activation.cpp"
rg -q '_pyc_group_0_valid' "${gate_dir}/group_activation.cpp"
if ! rg -q 'pyc_add_3 = .*\^' "${gate_dir}/group_activation.cpp" ||
   rg -q '^[[:space:]]+pyc_xor_2 = ' "${gate_dir}/group_activation.cpp"; then
  echo 'SimGraph single-use expression was not inlined into its consumer' >&2
  exit 1
fi
"${PYCC}" "${repo_root}/tests/newcircuit/group_activation.pyc" \
  --emit=cpp --sim-expression-inlining=false -o "${gate_dir}/group_inline_off.cpp"
rg -q '^[[:space:]]+pyc_xor_2 = ' "${gate_dir}/group_inline_off.cpp"
cat > "${gate_dir}/group_activation_harness.cpp" <<'CPP'
#include <sstream>
#include <string>
#include "group_activation.cpp"
int main() {
  pyc::gen::top sim;
  sim.a = pyc::cpp::Wire<8>(2);
  sim.b = pyc::cpp::Wire<8>(3);
  sim.eval();
  if (sim.y.value() != 8) return 1;
  sim.eval();
  sim.a = pyc::cpp::Wire<8>(4);
  sim.eval();
  if (sim.y.value() != 8) return 2;
  std::ostringstream stats;
  sim.dump_sim_stats(stats);
  if (stats.str().find("group_eval_calls=2\n") == std::string::npos) return 3;
  if (stats.str().find("group_cache_skips=1\n") == std::string::npos) return 4;
  return 0;
}
CPP
"${CXX:-c++}" -std=c++17 -I "${PYC_TOOLCHAIN_ROOT}/include" -I "${gate_dir}" \
  "${gate_dir}/group_activation_harness.cpp" -o "${gate_dir}/group_activation_harness"
PYC_SIM_STATS=1 "${gate_dir}/group_activation_harness"
"${PYCC}" "${repo_root}/tests/newcircuit/group_activation.pyc" \
  --emit=cpp --sim-group-activation=false -o "${gate_dir}/group_activation_off.cpp"
if rg -q '_pyc_group_0_valid' "${gate_dir}/group_activation_off.cpp"; then
  echo 'disabled group activation still emitted a group cache' >&2
  exit 1
fi

# Two consumers in one pure group can each inline a cheap expression when
# eliminating its materialized assignment does not increase modeled cost.
"${PYCC}" "${repo_root}/tests/newcircuit/multiuse_inline.pyc" \
  --emit=cpp -o "${gate_dir}/multiuse_inline_on.cpp"
"${PYCC}" "${repo_root}/tests/newcircuit/multiuse_inline.pyc" \
  --emit=cpp --sim-expression-inlining=false \
  -o "${gate_dir}/multiuse_inline_off.cpp"
! rg -q '^[[:space:]]+pyc_not_[0-9]+ = ' \
  "${gate_dir}/multiuse_inline_on.cpp"
rg -q '^[[:space:]]+pyc_not_[0-9]+ = ' \
  "${gate_dir}/multiuse_inline_off.cpp"
for mode in on off; do
  cp "${gate_dir}/multiuse_inline_${mode}.cpp" \
    "${gate_dir}/multiuse_inline_under_test.cpp"
  "${CXX:-c++}" -std=c++17 -I "${PYC_TOOLCHAIN_ROOT}/include" \
    -I "${gate_dir}" "${repo_root}/tests/newcircuit/multiuse_inline_harness.cpp" \
    -o "${gate_dir}/multiuse_inline_${mode}_harness"
  "${gate_dir}/multiuse_inline_${mode}_harness"
done

# Scalar arithmetic and comparison trees may inline up to the bounded host
# cost. A longer chain keeps a materialized cut to bound C++ expression size.
"${PYCC}" "${repo_root}/tests/newcircuit/cost_inline.pyc" \
  --emit=cpp -o "${gate_dir}/cost_inline_on.cpp"
"${PYCC}" "${repo_root}/tests/newcircuit/cost_inline.pyc" \
  --emit=cpp --sim-expression-inlining=false \
  -o "${gate_dir}/cost_inline_off.cpp"
! rg -q '^[[:space:]]+pyc_xor_[0-9]+ = ' \
  "${gate_dir}/cost_inline_on.cpp"
rg -q '^[[:space:]]+pyc_xor_[0-9]+ = ' \
  "${gate_dir}/cost_inline_off.cpp"
rg -q '^[[:space:]]+pyc_and_[0-9]+ = ' \
  "${gate_dir}/cost_inline_on.cpp"
for mode in on off; do
  cp "${gate_dir}/cost_inline_${mode}.cpp" \
    "${gate_dir}/cost_inline_under_test.cpp"
  "${CXX:-c++}" -std=c++17 -I "${PYC_TOOLCHAIN_ROOT}/include" \
    -I "${gate_dir}" "${repo_root}/tests/newcircuit/cost_inline_harness.cpp" \
    -o "${gate_dir}/cost_inline_${mode}_harness"
  "${gate_dir}/cost_inline_${mode}_harness"
done

"${PYCC}" "${repo_root}/tests/newcircuit/used_bit_activation.pyc" \
  --emit=cpp -o "${gate_dir}/used_bit.cpp"
rg -q 'a & pyc::cpp::Wire<8>({1ull})' "${gate_dir}/used_bit.cpp" -F
cat > "${gate_dir}/used_bit_harness.cpp" <<'CPP'
#include <sstream>
#include <string>
#include "used_bit.cpp"
int main() {
  pyc::gen::top sim;
  sim.a = pyc::cpp::Wire<8>(2);
  sim.b = pyc::cpp::Wire<8>(3);
  sim.eval();
  if (sim.y.value() != 1) return 1;
  sim.a = pyc::cpp::Wire<8>(4);
  sim.eval();
  if (sim.y.value() != 1) return 2;
  sim.a = pyc::cpp::Wire<8>(5);
  sim.eval();
  if (sim.y.value() != 0) return 3;
  std::ostringstream stats;
  sim.dump_sim_stats(stats);
  if (stats.str().find("group_eval_calls=2\n") == std::string::npos) return 4;
  if (stats.str().find("group_cache_skips=1\n") == std::string::npos) return 5;
  return 0;
}
CPP
"${CXX:-c++}" -std=c++17 -I "${PYC_TOOLCHAIN_ROOT}/include" -I "${gate_dir}" \
  "${gate_dir}/used_bit_harness.cpp" -o "${gate_dir}/used_bit_harness"
PYC_SIM_STATS=1 "${gate_dir}/used_bit_harness"
"${PYCC}" "${repo_root}/tests/newcircuit/constant_dynamic_shift_demand.pyc" \
  --emit=cpp -o "${gate_dir}/constant_dynamic_shift_demand.cpp"
rg -F -q 'a & pyc::cpp::Wire<16>({32771ull})' \
  "${gate_dir}/constant_dynamic_shift_demand.cpp"
rg -F -q 'pyc::cpp::ashr<16>' "${gate_dir}/constant_dynamic_shift_demand.cpp"
cat > "${gate_dir}/constant_dynamic_shift_demand_harness.cpp" <<'CPP'
#include <sstream>
#include <string>
#include "constant_dynamic_shift_demand.cpp"
int main() {
  pyc::gen::top sim;
  sim.a = pyc::cpp::Wire<16>(0);
  sim.eval();
  sim.a = pyc::cpp::Wire<16>(0x80);
  sim.eval();
  if (sim.left.value() || sim.logical.value() || sim.arithmetic.value()) return 1;
  sim.a = pyc::cpp::Wire<16>(1);
  sim.eval();
  if (sim.left.value() != 1 || sim.logical.value() || sim.arithmetic.value()) return 2;
  sim.a = pyc::cpp::Wire<16>(3);
  sim.eval();
  if (sim.left.value() != 1 || sim.logical.value() != 1 || sim.arithmetic.value()) return 3;
  sim.a = pyc::cpp::Wire<16>(0x8003);
  sim.eval();
  if (sim.left.value() != 1 || sim.logical.value() != 1 || sim.arithmetic.value() != 1) return 4;
  std::ostringstream stats;
  sim.dump_sim_stats(stats);
  if (stats.str().find("group_eval_calls=4\n") == std::string::npos ||
      stats.str().find("group_cache_skips=1\n") == std::string::npos) return 5;
  return 0;
}
CPP
"${CXX:-c++}" -std=c++17 -I "${PYC_TOOLCHAIN_ROOT}/include" -I "${gate_dir}" \
  "${gate_dir}/constant_dynamic_shift_demand_harness.cpp" \
  -o "${gate_dir}/constant_dynamic_shift_demand_harness"
PYC_SIM_STATS=1 "${gate_dir}/constant_dynamic_shift_demand_harness"
"${PYCC}" "${repo_root}/tests/newcircuit/ranged_dynamic_shift_demand.pyc" \
  --emit=cpp -o "${gate_dir}/ranged_dynamic_shift_demand.cpp"
rg -F -q 'a & pyc::cpp::Wire<16>({32783ull})' \
  "${gate_dir}/ranged_dynamic_shift_demand.cpp"
cat > "${gate_dir}/ranged_dynamic_shift_demand_harness.cpp" <<'CPP'
#include <sstream>
#include <string>
#include "ranged_dynamic_shift_demand.cpp"
int main() {
  pyc::gen::top sim;
  auto check = [&]() {
    unsigned a = sim.a.value(), shift = sim.amount.value();
    sim.eval();
    return sim.left.value() == (((a << shift) >> 1) & 1u) &&
           sim.logical.value() == ((a >> shift) & 1u) &&
           sim.arithmetic.value() == ((a >> 15) & 1u);
  };
  sim.a = pyc::cpp::Wire<16>(0);
  sim.amount = pyc::cpp::Wire<2>(0);
  if (!check()) return 1;
  sim.a = pyc::cpp::Wire<16>(0x80);
  if (!check()) return 2;
  sim.a = pyc::cpp::Wire<16>(3);
  if (!check()) return 3;
  sim.amount = pyc::cpp::Wire<2>(1);
  if (!check()) return 4;
  sim.amount = pyc::cpp::Wire<2>(2);
  if (!check()) return 5;
  sim.a = pyc::cpp::Wire<16>(0x8003);
  if (!check()) return 6;
  std::ostringstream stats;
  sim.dump_sim_stats(stats);
  if (stats.str().find("group_eval_calls=5\n") == std::string::npos ||
      stats.str().find("group_cache_skips=1\n") == std::string::npos) return 7;
  for (unsigned step = 0; step < 256; ++step) {
    sim.a = pyc::cpp::Wire<16>((step * 257u + 11u) & 65535u);
    sim.amount = pyc::cpp::Wire<2>(step & 3u);
    if (!check()) return 8;
  }
  return 0;
}
CPP
"${CXX:-c++}" -std=c++17 -I "${PYC_TOOLCHAIN_ROOT}/include" -I "${gate_dir}" \
  "${gate_dir}/ranged_dynamic_shift_demand_harness.cpp" \
  -o "${gate_dir}/ranged_dynamic_shift_demand_harness"
PYC_SIM_STATS=1 "${gate_dir}/ranged_dynamic_shift_demand_harness"
"${PYCC}" "${repo_root}/tests/newcircuit/wide_dynamic_shift_demand.pyc" \
  --emit=cpp -o "${gate_dir}/wide_dynamic_shift_demand.cpp"
rg -F -q 'a & pyc::cpp::Wire<64>({9223372041149743103ull})' \
  "${gate_dir}/wide_dynamic_shift_demand.cpp"
cat > "${gate_dir}/wide_dynamic_shift_demand_harness.cpp" <<'CPP'
#include <cstdint>
#include <sstream>
#include <string>
#include "wide_dynamic_shift_demand.cpp"
int main() {
  pyc::gen::top sim;
  auto check = [&]() {
    std::uint64_t a = sim.a.value();
    unsigned shift = sim.amount.value();
    sim.eval();
    return sim.left.value() == (((a << shift) >> 1) & 1ull) &&
           sim.logical.value() == ((a >> shift) & 1ull) &&
           sim.arithmetic.value() == (a >> 63);
  };
  sim.a = pyc::cpp::Wire<64>(0);
  sim.amount = pyc::cpp::Wire<5>(31);
  if (!check()) return 1;
  sim.a = pyc::cpp::Wire<64>(1ull << 40);
  if (!check()) return 2;
  sim.a = pyc::cpp::Wire<64>((1ull << 40) | (1ull << 31));
  if (!check()) return 3;
  std::ostringstream stats;
  sim.dump_sim_stats(stats);
  if (stats.str().find("group_eval_calls=2\n") == std::string::npos ||
      stats.str().find("group_cache_skips=1\n") == std::string::npos) return 4;
  for (unsigned step = 0; step < 256; ++step) {
    sim.a = pyc::cpp::Wire<64>(
        (std::uint64_t(step * 257u + 11u) << 32) |
        std::uint64_t(step * 65537u + 7u));
    sim.amount = pyc::cpp::Wire<5>(step & 31u);
    if (!check()) return 5;
  }
  return 0;
}
CPP
"${CXX:-c++}" -std=c++17 -I "${PYC_TOOLCHAIN_ROOT}/include" -I "${gate_dir}" \
  "${gate_dir}/wide_dynamic_shift_demand_harness.cpp" \
  -o "${gate_dir}/wide_dynamic_shift_demand_harness"
PYC_SIM_STATS=1 "${gate_dir}/wide_dynamic_shift_demand_harness"
"${PYCC}" "${repo_root}/tests/newcircuit/comb_bit_demand.pyc" \
  --emit=cpp -o "${gate_dir}/comb_bit_demand.cpp"
rg -F -q '_pyc_group_0_in_0 == (a & pyc::cpp::Wire<16>({1ull}))' \
  "${gate_dir}/comb_bit_demand.cpp"
cat > "${gate_dir}/comb_bit_demand_harness.cpp" <<'CPP'
#include <sstream>
#include <string>
#include "comb_bit_demand.cpp"
int main() {
  pyc::gen::top sim;
  sim.a = pyc::cpp::Wire<16>(0);
  sim.b = pyc::cpp::Wire<16>(0);
  sim.c = pyc::cpp::Wire<16>(0);
  sim.eval();
  if (sim.bit.value() != 0) return 1;
  sim.a = pyc::cpp::Wire<16>(2);
  sim.eval();
  if (sim.bit.value() != 0) return 2;
  sim.a = pyc::cpp::Wire<16>(3);
  sim.eval();
  if (sim.bit.value() != 1) return 3;
  std::ostringstream stats;
  sim.dump_sim_stats(stats);
  if (stats.str().find("group_eval_calls=4\n") == std::string::npos) return 4;
  if (stats.str().find("group_cache_skips=2\n") == std::string::npos) return 5;
  return 0;
}
CPP
"${CXX:-c++}" -std=c++17 -I "${PYC_TOOLCHAIN_ROOT}/include" -I "${gate_dir}" \
  "${gate_dir}/comb_bit_demand_harness.cpp" -o "${gate_dir}/comb_bit_demand_harness"
PYC_SIM_STATS=1 "${gate_dir}/comb_bit_demand_harness"
"${PYCC}" "${repo_root}/tests/newcircuit/sparse_bit_activation.pyc" \
  --emit=cpp -o "${gate_dir}/sparse_bit.cpp"
rg -q 'a & pyc::cpp::Wire<8>({128ull})' "${gate_dir}/sparse_bit.cpp" -F
cat > "${gate_dir}/sparse_bit_harness.cpp" <<'CPP'
#include <sstream>
#include <string>
#include "sparse_bit.cpp"
int main() {
  pyc::gen::top sim;
  sim.a = pyc::cpp::Wire<8>(0x80);
  sim.b = pyc::cpp::Wire<8>(0);
  sim.eval();
  if (sim.y.value() != 1) return 1;
  sim.a = pyc::cpp::Wire<8>(0x81);
  sim.eval();
  if (sim.y.value() != 1) return 2;
  sim.a = pyc::cpp::Wire<8>(0x01);
  sim.eval();
  if (sim.y.value() != 0) return 3;
  sim.b = pyc::cpp::Wire<8>(0x01);
  sim.eval();
  if (sim.y.value() != 0) return 4;
  std::ostringstream stats;
  sim.dump_sim_stats(stats);
  if (stats.str().find("group_eval_calls=2\n") == std::string::npos) return 5;
  if (stats.str().find("group_cache_skips=2\n") == std::string::npos) return 6;
  return 0;
}
CPP
"${CXX:-c++}" -std=c++17 -I "${PYC_TOOLCHAIN_ROOT}/include" -I "${gate_dir}" \
  "${gate_dir}/sparse_bit_harness.cpp" -o "${gate_dir}/sparse_bit_harness"
PYC_SIM_STATS=1 "${gate_dir}/sparse_bit_harness"
"${PYCC}" "${repo_root}/tests/newcircuit/vector_group_activation.pyc" \
  --emit=cpp -o "${gate_dir}/vector_group.cpp"
cat > "${gate_dir}/vector_group_harness.cpp" <<'CPP'
#include <sstream>
#include <string>
#include "vector_group.cpp"
int main() {
  pyc::gen::top sim;
  sim.a = pyc::cpp::Wire<8>(1);
  sim.b = pyc::cpp::Wire<8>(2);
  sim.eval();
  if (sim.y[0].value() != 3 || sim.y[1].value() != 3) return 1;
  sim.eval();
  sim.a = pyc::cpp::Wire<8>(2);
  sim.eval();
  if (sim.y[0].value() != 4 || sim.y[1].value() != 4) return 2;
  std::ostringstream stats;
  sim.dump_sim_stats(stats);
  if (stats.str().find("group_eval_calls=2\n") == std::string::npos) return 3;
  if (stats.str().find("group_cache_skips=1\n") == std::string::npos) return 4;
  return 0;
}
CPP
"${CXX:-c++}" -std=c++17 -I "${PYC_TOOLCHAIN_ROOT}/include" -I "${gate_dir}" \
  "${gate_dir}/vector_group_harness.cpp" -o "${gate_dir}/vector_group_harness"
PYC_SIM_STATS=1 "${gate_dir}/vector_group_harness"

# A one-operation group formed by bounded partition still receives activation.
# A fixed-index vector read compares only its observed outer lane.
"${PYCC}" "${repo_root}/tests/newcircuit/vector_lane_activity.pyc" \
  --emit=cpp --sim-supernode-max-size=1 --sim-replication=false \
  -o "${gate_dir}/vector_lane_activity.cpp"
rg -q '_pyc_group_0_in_0\[1\] == v\[1\]' \
  "${gate_dir}/vector_lane_activity.cpp"
rg -q '_pyc_group_0_in_0\[1\] = v\[1\];' \
  "${gate_dir}/vector_lane_activity.cpp"
! rg -q '_pyc_group_0_in_0 = v;' \
  "${gate_dir}/vector_lane_activity.cpp"
"${PYCC}" "${repo_root}/tests/newcircuit/vector_lane_activity.pyc" \
  --emit=cpp --sim-supernode-max-size=1 --sim-replication=false \
  --sim-used-bit-activation=false \
  -o "${gate_dir}/vector_lane_activity_full.cpp"
rg -q '_pyc_group_0_in_0 == v' \
  "${gate_dir}/vector_lane_activity_full.cpp"
cat > "${gate_dir}/vector_lane_activity_harness.cpp" <<'CPP'
#include <sstream>
#include <string>
#include "vector_lane_activity.cpp"
int main() {
  pyc::gen::top sim;
  for (unsigned i = 0; i < 4; ++i)
    sim.v[i] = pyc::cpp::Wire<8>(10 + i);
  sim.eval();
  if (sim.y.value() != 16) return 1;
  sim.v[3] = pyc::cpp::Wire<8>(99);
  sim.eval();
  if (sim.y.value() != 16) return 2;
  sim.v[1] = pyc::cpp::Wire<8>(25);
  sim.eval();
  if (sim.y.value() != 30) return 3;
  std::ostringstream stats;
  sim.dump_sim_stats(stats);
  if (stats.str().find("group_eval_calls=4\n") == std::string::npos) return 4;
  if (stats.str().find("group_cache_skips=2\n") == std::string::npos) return 5;
  return 0;
}
CPP
"${CXX:-c++}" -std=c++17 -I "${PYC_TOOLCHAIN_ROOT}/include" -I "${gate_dir}" \
  "${gate_dir}/vector_lane_activity_harness.cpp" -o "${gate_dir}/vector_lane_activity_harness"
PYC_SIM_STATS=1 "${gate_dir}/vector_lane_activity_harness"

# The selected column of a rank-2 vector controls activity element by element.
"${PYCC}" "${repo_root}/tests/newcircuit/rank2_element_activity.pyc" \
  --emit=cpp -o "${gate_dir}/rank2_element_activity.cpp"
rg -q '\[0\]\[2\] == v\[0\]\[2\]' \
  "${gate_dir}/rank2_element_activity.cpp"
rg -q '\[0\]\[2\] = v\[0\]\[2\];' \
  "${gate_dir}/rank2_element_activity.cpp"
! rg -q '_pyc_group_0_in_0 = v;' \
  "${gate_dir}/rank2_element_activity.cpp"
"${CXX:-c++}" -std=c++17 -I "${PYC_TOOLCHAIN_ROOT}/include" \
  -I "${gate_dir}" "${repo_root}/tests/newcircuit/rank2_element_activity_harness.cpp" \
  -o "${gate_dir}/rank2_element_activity_harness"
PYC_SIM_STATS=1 "${gate_dir}/rank2_element_activity_harness"

# Keep the vector reduction here to exercise packed propagation across a
# selected-lane reader; the separate reduction fixture checks lane lowering.
"${PYCC}" "${repo_root}/tests/newcircuit/vector_lane_propagation.pyc" \
  --emit=cpp --sim-supernode-max-size=1 --sim-replication=false \
  -o "${gate_dir}/vector_lane_propagation.cpp"
rg -q '\[0\] != _pyc_old_group_value_[0-9]+\[0\]' \
  "${gate_dir}/vector_lane_propagation.cpp"
cat > "${gate_dir}/vector_lane_propagation_harness.cpp" <<'CPP'
#include <sstream>
#include <string>
#include "vector_lane_propagation.cpp"
int main() {
  pyc::gen::top sim;
  sim.v[0][0] = pyc::cpp::Wire<8>(7);
  sim.v[0][1] = pyc::cpp::Wire<8>(0);
  sim.v[0][2] = pyc::cpp::Wire<8>(0);
  sim.v[1][0] = pyc::cpp::Wire<8>(9);
  sim.v[1][1] = pyc::cpp::Wire<8>(0);
  sim.v[1][2] = pyc::cpp::Wire<8>(0);
  sim.eval();
  if (sim.y.value() != 12) return 1;
  sim.v[1][0] = pyc::cpp::Wire<8>(25);
  sim.eval();
  if (sim.y.value() != 12) return 2;
  sim.v[0][0] = pyc::cpp::Wire<8>(11);
  sim.eval();
  if (sim.y.value() != 16) return 3;
  std::ostringstream stats;
  sim.dump_sim_stats(stats);
  if (stats.str().find("group_eval_calls=7\n") == std::string::npos) return 4;
  if (stats.str().find("group_cache_skips=2\n") == std::string::npos) return 5;
  return 0;
}
CPP
"${CXX:-c++}" -std=c++17 -I "${PYC_TOOLCHAIN_ROOT}/include" -I "${gate_dir}" \
  "${gate_dir}/vector_lane_propagation_harness.cpp" -o "${gate_dir}/vector_lane_propagation_harness"
PYC_SIM_STATS=1 "${gate_dir}/vector_lane_propagation_harness"
"${PYCC}" "${repo_root}/tests/newcircuit/used_bit_activation.pyc" \
  --emit=cpp --sim-used-bit-activation=false -o "${gate_dir}/used_bit_off.cpp"
if rg -q 'a & pyc::cpp::Wire<8>({1ull})' "${gate_dir}/used_bit_off.cpp" -F; then
  echo 'disabled used-bit activation still emitted a bit mask' >&2
  exit 1
fi

# A downstream one-bit observation must narrow activation in both partitions,
# including an upstream supernode whose own live-out is eight bits wide. The
# fixture keeps its slice explicit so the Hardware MLIR shift-slice rewrite
# does not replace the intended cross-group graph test.
"${PYCC}" "${repo_root}/tests/newcircuit/cross_group_used_bits.pyc" \
  --emit=cpp --sim-supernode-max-size=2 -o "${gate_dir}/cross_group_bits.cpp"
rg -q 'pyc_shli_2 & pyc::cpp::Wire<8>({4ull})' \
  "${gate_dir}/cross_group_bits.cpp" -F
rg -q 'a & pyc::cpp::Wire<8>({1ull})' \
  "${gate_dir}/cross_group_bits.cpp" -F
cat > "${gate_dir}/cross_group_bits_harness.cpp" <<'CPP'
#include <sstream>
#include <string>
#include "cross_group_bits.cpp"
int main() {
  pyc::gen::top sim;
  sim.a = pyc::cpp::Wire<8>(2);
  sim.b = pyc::cpp::Wire<8>(0);
  sim.eval();
  if (sim.y.value() != 0) return 1;
  sim.a = pyc::cpp::Wire<8>(4);
  sim.eval();
  if (sim.y.value() != 0) return 2;
  sim.a = pyc::cpp::Wire<8>(5);
  sim.eval();
  if (sim.y.value() != 1) return 3;
  std::ostringstream stats;
  sim.dump_sim_stats(stats);
  if (stats.str().find("group_eval_calls=6\n") == std::string::npos) return 4;
  if (stats.str().find("group_cache_skips=3\n") == std::string::npos) return 5;
  return 0;
}
CPP
"${CXX:-c++}" -std=c++17 -I "${PYC_TOOLCHAIN_ROOT}/include" -I "${gate_dir}" \
  "${gate_dir}/cross_group_bits_harness.cpp" -o "${gate_dir}/cross_group_bits_harness"
PYC_SIM_STATS=1 "${gate_dir}/cross_group_bits_harness"

# A changed root input can leave its output unchanged. The packed activity
# bit must then skip the downstream group without losing a later real change.
"${PYCC}" "${repo_root}/tests/newcircuit/packed_activity.pyc" \
  --emit=cpp --sim-supernode-max-size=2 -o "${gate_dir}/packed_activity.cpp"
rg -q '_pyc_group_active_flags' "${gate_dir}/packed_activity.cpp"
rg -q 'pyc_add_2 & pyc::cpp::Wire<8>({127ull})) != (_pyc_old_group_value_' \
  "${gate_dir}/packed_activity.cpp" -F
cat > "${gate_dir}/packed_activity_harness.cpp" <<'CPP'
#include "packed_activity.cpp"
#include <sstream>
#include <string>
int main() {
  pyc::gen::top sim;
  sim.a = pyc::cpp::Wire<8>(0x12);
  sim.b = pyc::cpp::Wire<8>(0);
  sim.c = pyc::cpp::Wire<8>(5);
  sim.eval();
  if (sim.out.value() != 0xf4) return 1;
  sim.a = pyc::cpp::Wire<8>(0x83);
  sim.eval();
  if (sim.out.value() != 0xf4) return 2;
  std::ostringstream stats;
  sim.dump_sim_stats(stats);
  if (stats.str().find("group_eval_calls=3\n") == std::string::npos) return 3;
  if (stats.str().find("group_cache_skips=1\n") == std::string::npos) return 4;
  sim.b = pyc::cpp::Wire<8>(255);
  sim.eval();
  if (sim.out.value() != ((~(0x83 + 5) << 1) & 255)) return 5;
  return 0;
}
CPP
"${CXX:-c++}" -std=c++17 -I "${PYC_TOOLCHAIN_ROOT}/include" -I "${gate_dir}" \
  "${gate_dir}/packed_activity_harness.cpp" -o "${gate_dir}/packed_activity_harness"
PYC_SIM_STATS=1 "${gate_dir}/packed_activity_harness"

# A producer observed at full width can change outside the downstream
# consumer's demanded bit. That must leave the packed consumer inactive.
"${PYCC}" "${repo_root}/tests/newcircuit/masked_propagation.pyc" \
  --emit=cpp --sim-supernode-max-size=2 -o "${gate_dir}/masked_propagation.cpp"
rg -q 'pyc_and_2 & pyc::cpp::Wire<8>({1ull})) != (_pyc_old_group_value_' \
  "${gate_dir}/masked_propagation.cpp" -F
cat > "${gate_dir}/masked_propagation_harness.cpp" <<'CPP'
#include "masked_propagation.cpp"
#include <sstream>
#include <string>
int main() {
  pyc::gen::top sim;
  sim.a = pyc::cpp::Wire<8>(0);
  sim.b = pyc::cpp::Wire<8>(0);
  sim.c = pyc::cpp::Wire<8>(255);
  sim.eval();
  if (sim.root.value() != 0 || sim.y.value() != 1) return 1;
  sim.a = pyc::cpp::Wire<8>(2);
  sim.eval();
  if (sim.root.value() != 2 || sim.y.value() != 1) return 2;
  std::ostringstream stats;
  sim.dump_sim_stats(stats);
  if (stats.str().find("group_eval_calls=3\n") == std::string::npos) return 3;
  if (stats.str().find("group_cache_skips=1\n") == std::string::npos) return 4;
  sim.a = pyc::cpp::Wire<8>(3);
  sim.eval();
  if (sim.root.value() != 3 || sim.y.value() != 0) return 5;
  return 0;
}
CPP
"${CXX:-c++}" -std=c++17 -I "${PYC_TOOLCHAIN_ROOT}/include" -I "${gate_dir}" \
  "${gate_dir}/masked_propagation_harness.cpp" -o "${gate_dir}/masked_propagation_harness"
PYC_SIM_STATS=1 "${gate_dir}/masked_propagation_harness"

"${PYCC}" "${repo_root}/tests/newcircuit/masked_propagation_wide.pyc" \
  --emit=cpp --sim-supernode-max-size=2 -o "${gate_dir}/masked_propagation_wide.cpp"
rg -q 'pyc::cpp::Wire<130>({0ull, 2ull, 0ull})' \
  "${gate_dir}/masked_propagation_wide.cpp" -F
cat > "${gate_dir}/masked_propagation_wide_harness.cpp" <<'CPP'
#include "masked_propagation_wide.cpp"
#include <sstream>
#include <string>
int main() {
  pyc::gen::top sim;
  sim.a = pyc::cpp::Wire<130>({0ull, 0ull, 0ull});
  sim.b = pyc::cpp::Wire<130>({0ull, 0ull, 0ull});
  sim.c = pyc::cpp::Wire<130>({~0ull, ~0ull, 3ull});
  sim.eval();
  if (sim.root.word(1) != 0 || sim.y.value() != 1) return 1;
  sim.a = pyc::cpp::Wire<130>({0ull, 1ull, 0ull});
  sim.eval();
  if (sim.root.word(1) != 1 || sim.y.value() != 1) return 2;
  std::ostringstream stats;
  sim.dump_sim_stats(stats);
  if (stats.str().find("group_eval_calls=3\n") == std::string::npos ||
      stats.str().find("group_cache_skips=1\n") == std::string::npos) return 3;
  sim.a = pyc::cpp::Wire<130>({0ull, 2ull, 0ull});
  sim.eval();
  if (sim.root.word(1) != 2 || sim.y.value() != 0) return 4;
  return 0;
}
CPP
"${CXX:-c++}" -std=c++17 -I "${PYC_TOOLCHAIN_ROOT}/include" -I "${gate_dir}" \
  "${gate_dir}/masked_propagation_wide_harness.cpp" -o "${gate_dir}/masked_propagation_wide_harness"
PYC_SIM_STATS=1 "${gate_dir}/masked_propagation_wide_harness"

# A constant bitwise mask removes input-bit demand through the whole group.
"${PYCC}" "${repo_root}/tests/newcircuit/constant_mask_activity.pyc" \
  --emit=cpp --sim-supernode-max-size=3 -o "${gate_dir}/constant_mask_activity.cpp"
rg -q 'a & pyc::cpp::Wire<8>({254ull})' \
  "${gate_dir}/constant_mask_activity.cpp" -F
rg -q 'b & pyc::cpp::Wire<8>({254ull})' \
  "${gate_dir}/constant_mask_activity.cpp" -F
cat > "${gate_dir}/constant_mask_activity_harness.cpp" <<'CPP'
#include "constant_mask_activity.cpp"
#include <sstream>
#include <string>
int main() {
  pyc::gen::top sim;
  sim.a = pyc::cpp::Wire<8>(0);
  sim.b = pyc::cpp::Wire<8>(0);
  sim.eval();
  if (sim.out.value() != 255) return 1;
  sim.a = pyc::cpp::Wire<8>(1);
  sim.eval();
  if (sim.out.value() != 255) return 2;
  std::ostringstream stats;
  sim.dump_sim_stats(stats);
  if (stats.str().find("group_eval_calls=1\n") == std::string::npos ||
      stats.str().find("group_cache_skips=1\n") == std::string::npos) return 3;
  sim.a = pyc::cpp::Wire<8>(2);
  sim.eval();
  if (sim.out.value() != 253) return 4;
  return 0;
}
CPP
"${CXX:-c++}" -std=c++17 -I "${PYC_TOOLCHAIN_ROOT}/include" -I "${gate_dir}" \
  "${gate_dir}/constant_mask_activity_harness.cpp" -o "${gate_dir}/constant_mask_activity_harness"
PYC_SIM_STATS=1 "${gate_dir}/constant_mask_activity_harness"

# An expensive scalar node receives activity checks even if partitioning
# leaves it as a singleton.
"${PYCC}" "${repo_root}/tests/newcircuit/singleton_div_activity.pyc" \
  --emit=cpp -o "${gate_dir}/singleton_div_activity.cpp"
"${PYCC}" "${repo_root}/tests/newcircuit/singleton_div_activity.pyc" \
  --emit=cpp --sim-group-activation=false -o "${gate_dir}/singleton_div_activity_off.cpp"
rg -q 'inline void eval_sim_group_0()' "${gate_dir}/singleton_div_activity.cpp" -F
if rg -q 'inline void eval_sim_group_0()' "${gate_dir}/singleton_div_activity_off.cpp" -F; then
  echo 'disabled activity still grouped the singleton division' >&2
  exit 1
fi
cat > "${gate_dir}/singleton_div_activity_harness.cpp" <<'CPP'
#include "singleton_div_activity.cpp"
#include <sstream>
#include <string>
int main() {
  pyc::gen::top sim;
  sim.a = pyc::cpp::Wire<8>(8);
  sim.b = pyc::cpp::Wire<8>(2);
  sim.eval();
  if (sim.out.value() != 4) return 1;
  sim.eval();
  if (sim.out.value() != 4) return 2;
  std::ostringstream stats;
  sim.dump_sim_stats(stats);
  if (stats.str().find("group_eval_calls=1\n") == std::string::npos ||
      stats.str().find("group_cache_skips=1\n") == std::string::npos) return 3;
  sim.a = pyc::cpp::Wire<8>(10);
  sim.eval();
  if (sim.out.value() != 5) return 4;
  return 0;
}
CPP
"${CXX:-c++}" -std=c++17 -I "${PYC_TOOLCHAIN_ROOT}/include" -I "${gate_dir}" \
  "${gate_dir}/singleton_div_activity_harness.cpp" -o "${gate_dir}/singleton_div_activity_harness"
PYC_SIM_STATS=1 "${gate_dir}/singleton_div_activity_harness"

# A cheap pure singleton is also a supernode and can skip evaluation when its
# inputs are unchanged. Disabling activation preserves eager execution.
cat > "${gate_dir}/singleton_add_activity_harness.cpp" <<'CPP'
#include "singleton_add_under_test.cpp"
#include <sstream>
#include <string>
int main() {
  pyc::gen::top sim;
  sim.a = pyc::cpp::Wire<8>(2);
  sim.b = pyc::cpp::Wire<8>(3);
  sim.eval();
  if (sim.y.value() != 5) return 1;
  sim.eval();
  if (sim.y.value() != 5) return 2;
  sim.a = pyc::cpp::Wire<8>(3);
  sim.eval();
  if (sim.y.value() != 6) return 3;
#if ACTIVITY_ENABLED
  std::ostringstream stats;
  sim.dump_sim_stats(stats);
  if (stats.str().find("group_eval_calls=2\n") == std::string::npos ||
      stats.str().find("group_cache_skips=1\n") == std::string::npos) return 4;
#endif
  return 0;
}
CPP
for mode in on off; do
  args=()
  enabled=1
  if [[ "${mode}" == off ]]; then
    args+=(--sim-group-activation=false)
    enabled=0
  fi
  "${PYCC}" "${repo_root}/tests/newcircuit/singleton_add_activity.pyc" \
    --emit=cpp "${args[@]}" -o "${gate_dir}/singleton_add_${mode}.cpp"
  if [[ "${mode}" == on ]]; then
    rg -F -q 'inline void eval_sim_group_0()' \
      "${gate_dir}/singleton_add_${mode}.cpp"
  else
    ! rg -F -q 'inline void eval_sim_group_0()' \
      "${gate_dir}/singleton_add_${mode}.cpp"
  fi
  cp "${gate_dir}/singleton_add_${mode}.cpp" \
    "${gate_dir}/singleton_add_under_test.cpp"
  "${CXX:-c++}" -std=c++17 -I "${PYC_TOOLCHAIN_ROOT}/include" -I "${gate_dir}" \
    -DACTIVITY_ENABLED="${enabled}" \
    "${gate_dir}/singleton_add_activity_harness.cpp" \
    -o "${gate_dir}/singleton_add_${mode}_harness"
  PYC_SIM_STATS=1 "${gate_dir}/singleton_add_${mode}_harness"
done

# Six scalar muxes sharing a selector use one branch in a group. A chunk
# boundary safely falls back to individual graph mux expressions.
"${PYCC}" "${repo_root}/tests/newcircuit/mux_condition_batch.pyc" \
  --emit=cpp -o "${gate_dir}/mux_condition_batch.cpp"
"${PYCC}" "${repo_root}/tests/newcircuit/mux_condition_batch.pyc" \
  --emit=cpp --cpp-shard-max-ast-nodes=3 \
  -o "${gate_dir}/mux_condition_batch_chunk.cpp"
rg -q 'if (sel.toBool())' "${gate_dir}/mux_condition_batch.cpp" -F
test "$(rg -c 'pyc::cpp::mux<8>' \
  "${gate_dir}/mux_condition_batch_chunk.cpp" -F)" = 6
cat > "${gate_dir}/mux_condition_batch_harness.cpp" <<'CPP'
#ifdef PYC_CHUNKED
#include "mux_condition_batch_chunk.cpp"
#else
#include "mux_condition_batch.cpp"
#endif
int main() {
  pyc::gen::top sim;
  pyc::cpp::Wire<8> *a[] = {&sim.a0, &sim.a1, &sim.a2, &sim.a3, &sim.a4, &sim.a5};
  pyc::cpp::Wire<8> *b[] = {&sim.b0, &sim.b1, &sim.b2, &sim.b3, &sim.b4, &sim.b5};
  pyc::cpp::Wire<8> *out[] = {&sim.o0, &sim.o1, &sim.o2, &sim.o3, &sim.o4, &sim.o5};
  for (unsigned i = 0; i < 6; ++i) {
    *a[i] = pyc::cpp::Wire<8>(i + 10);
    *b[i] = pyc::cpp::Wire<8>(i + 100);
  }
  sim.sel = pyc::cpp::Wire<1>(0);
  sim.eval();
  for (unsigned i = 0; i < 6; ++i)
    if (out[i]->value() != i + 100) return 1;
  sim.sel = pyc::cpp::Wire<1>(1);
  sim.eval();
  for (unsigned i = 0; i < 6; ++i)
    if (out[i]->value() != i + 10) return 2;
  *a[5] = pyc::cpp::Wire<8>(77);
  sim.eval();
  return sim.o5.value() == 77 ? 0 : 3;
}
CPP
"${CXX:-c++}" -std=c++17 -I "${PYC_TOOLCHAIN_ROOT}/include" -I "${gate_dir}" \
  "${gate_dir}/mux_condition_batch_harness.cpp" -o "${gate_dir}/mux_condition_batch_harness"
"${CXX:-c++}" -std=c++17 -DPYC_CHUNKED \
  -I "${PYC_TOOLCHAIN_ROOT}/include" -I "${gate_dir}" \
  "${gate_dir}/mux_condition_batch_harness.cpp" -o "${gate_dir}/mux_condition_batch_chunk_harness"
"${gate_dir}/mux_condition_batch_harness"
"${gate_dir}/mux_condition_batch_chunk_harness"

# Independent expressions between the muxes may move after their shared
# condition branch when all mux operands are already available.
"${PYCC}" "${repo_root}/tests/newcircuit/mux_condition_interleaved.pyc" \
  --emit=cpp -o "${gate_dir}/mux_condition_interleaved.cpp"
test "$(rg -F -c 'if (sel.toBool())' "${gate_dir}/mux_condition_interleaved.cpp")" = 1
cat > "${gate_dir}/mux_condition_interleaved_harness.cpp" <<'CPP'
#include "mux_condition_interleaved.cpp"
int main() {
  pyc::gen::top sim;
  pyc::cpp::Wire<8> *a[] = {&sim.a0, &sim.a1, &sim.a2, &sim.a3, &sim.a4, &sim.a5};
  pyc::cpp::Wire<8> *b[] = {&sim.b0, &sim.b1, &sim.b2, &sim.b3, &sim.b4, &sim.b5};
  pyc::cpp::Wire<8> *out[] = {&sim.o0, &sim.o1, &sim.o2, &sim.o3, &sim.o4, &sim.o5};
  pyc::cpp::Wire<8> *inv[] = {&sim.n0, &sim.n1, &sim.n2, &sim.n3, &sim.n4, &sim.n5};
  for (unsigned step = 0; step < 64; ++step) {
    sim.sel = pyc::cpp::Wire<1>(step & 1u);
    for (unsigned i = 0; i < 6; ++i) {
      *a[i] = pyc::cpp::Wire<8>((step * 7u + i * 13u) & 255u);
      *b[i] = pyc::cpp::Wire<8>((step * 11u + i * 17u) & 255u);
    }
    sim.eval();
    for (unsigned i = 0; i < 6; ++i) {
      unsigned av = a[i]->value(), bv = b[i]->value();
      if (out[i]->value() != ((step & 1u) ? av : bv) ||
          inv[i]->value() != ((~av) & 255u)) return 1;
    }
  }
  return 0;
}
CPP
"${CXX:-c++}" -std=c++17 -I "${PYC_TOOLCHAIN_ROOT}/include" -I "${gate_dir}" \
  "${gate_dir}/mux_condition_interleaved_harness.cpp" \
  -o "${gate_dir}/mux_condition_interleaved_harness"
"${gate_dir}/mux_condition_interleaved_harness"
"${PYCC}" "${repo_root}/tests/newcircuit/mux_condition_interleaved.pyc" \
  --emit=cpp --sim-supernode-max-size=6 \
  -o "${gate_dir}/mux_condition_interleaved.cpp"
test "$(rg -F -c 'if (sel.toBool())' "${gate_dir}/mux_condition_interleaved.cpp")" = 1
rg -F -q 'inline void eval_sim_group_1()' \
  "${gate_dir}/mux_condition_interleaved.cpp"
"${CXX:-c++}" -std=c++17 -I "${PYC_TOOLCHAIN_ROOT}/include" -I "${gate_dir}" \
  "${gate_dir}/mux_condition_interleaved_harness.cpp" \
  -o "${gate_dir}/mux_condition_interleaved_bounded_harness"
"${gate_dir}/mux_condition_interleaved_bounded_harness"

# Global initial partition joins adjacent pure DAG components when the
# bounded GSIM interval cost does not require a cut.
"${PYCC}" "${repo_root}/tests/newcircuit/interleaved_dag.pyc" \
  --emit=cpp -o "${gate_dir}/interleaved_dag.cpp"
test "$(rg -c 'inline void eval_sim_group_[0-9]+\(' "${gate_dir}/interleaved_dag.cpp")" = 1
cat > "${gate_dir}/interleaved_dag_harness.cpp" <<'CPP'
#include <sstream>
#include <string>
#include "interleaved_dag.cpp"
int main() {
  pyc::gen::top sim;
  sim.a = pyc::cpp::Wire<8>(1);
  sim.b = pyc::cpp::Wire<8>(2);
  sim.c = pyc::cpp::Wire<8>(3);
  sim.d = pyc::cpp::Wire<8>(4);
  sim.eval();
  if (sim.y.value() != 4 || sim.z.value() != 10) return 1;
  sim.a = pyc::cpp::Wire<8>(2);
  sim.eval();
  if (sim.y.value() != 6 || sim.z.value() != 10) return 2;
  sim.c = pyc::cpp::Wire<8>(4);
  sim.eval();
  if (sim.y.value() != 6 || sim.z.value() != 12) return 3;
  std::ostringstream stats;
  sim.dump_sim_stats(stats);
  if (stats.str().find("group_eval_calls=3\n") == std::string::npos) return 4;
  if (stats.str().find("group_cache_skips=0\n") == std::string::npos) return 5;
  return 0;
}
CPP
"${CXX:-c++}" -std=c++17 -I "${PYC_TOOLCHAIN_ROOT}/include" -I "${gate_dir}" \
  "${gate_dir}/interleaved_dag_harness.cpp" -o "${gate_dir}/interleaved_dag_harness"
PYC_SIM_STATS=1 "${gate_dir}/interleaved_dag_harness"

# In-degree-one DAG coarsening may join pure expressions across an unrelated
# pyc.comb boundary. Both expressions still evaluate together in one group.
"${PYCC}" "${repo_root}/tests/newcircuit/coarsen_across_comb.pyc" \
  --emit=cpp --sim-supernode-max-size=0 -o "${gate_dir}/coarsen.cpp"
"${PYCC}" "${repo_root}/tests/newcircuit/coarsen_across_comb.pyc" \
  --emit=cpp --cpp-only-preserve-ops -o "${gate_dir}/coarsen_preserve.cpp"
python3 - "${gate_dir}/coarsen.cpp" <<'PY'
from pathlib import Path
import sys
text = Path(sys.argv[1]).read_text(encoding="utf-8")
group = text.split("inline void eval_sim_group_0()", 1)[1].split("inline void eval_comb_pass()", 1)[0]
assert "(a + b)" in group and "pyc_xor_" in group and "pyc_or_" in group
PY
cat > "${gate_dir}/coarsen_harness.cpp" <<'CPP'
#include "coarsen.cpp"
int main() {
  pyc::gen::top sim;
  for (unsigned a = 0; a < 8; ++a)
    for (unsigned b = 0; b < 8; ++b) {
      sim.a = pyc::cpp::Wire<8>(a);
      sim.b = pyc::cpp::Wire<8>(b);
      sim.c = pyc::cpp::Wire<8>(a + b);
      sim.eval();
      unsigned p = (a + b) & 255;
      if (sim.x.value() != (p ^ a) || sim.y.value() != (p | b) ||
          sim.z.value() != ((~(a + b)) & 255)) return 1;
    }
  return 0;
}
CPP
sed 's/"coarsen.cpp"/"coarsen_preserve.cpp"/' \
  "${gate_dir}/coarsen_harness.cpp" > "${gate_dir}/coarsen_preserve_harness.cpp"
for variant in coarsen coarsen_preserve; do
  "${CXX:-c++}" -std=c++17 -I "${PYC_TOOLCHAIN_ROOT}/include" -I "${gate_dir}" \
    "${gate_dir}/${variant}_harness.cpp" -o "${gate_dir}/${variant}_harness"
  "${gate_dir}/${variant}_harness"
done

# Equal nonempty predecessor sets are merged as GSIM sibling supernodes.
"${PYCC}" "${repo_root}/tests/newcircuit/coarsen_siblings.pyc" \
  --emit=cpp -o "${gate_dir}/coarsen_siblings.cpp"
python3 - "${gate_dir}/coarsen_siblings.cpp" <<'PY'
from pathlib import Path
import sys
text = Path(sys.argv[1]).read_text(encoding="utf-8")
group = text.split("inline void eval_sim_group_0()", 1)[1].split("inline void eval_comb_pass()", 1)[0]
assert "pyc_xor_" in group and "pyc_or_" in group
PY

# GSIM-style replication removes a cheap singleton evaluation and copies its
# expression into two separate consumer supernodes. Compare enabled/disabled
# generated simulations over several input changes.
"${PYCC}" "${repo_root}/tests/newcircuit/replicated_seed.pyc" \
  --emit=cpp --sim-supernode-max-size=0 -o "${gate_dir}/replicated_seed.cpp"
"${PYCC}" "${repo_root}/tests/newcircuit/replicated_seed.pyc" \
  --emit=cpp --sim-supernode-max-size=0 --sim-replication=false -o "${gate_dir}/replicated_seed_off.cpp"
if rg -q '^[[:space:]]+pyc_xor_1 = ' "${gate_dir}/replicated_seed.cpp"; then
  echo 'replicated singleton still has a standalone assignment' >&2
  exit 1
fi
rg -q '^[[:space:]]+pyc_xor_1 = ' "${gate_dir}/replicated_seed_off.cpp"
test "$(rg -c '\^ b' "${gate_dir}/replicated_seed.cpp")" = 2
cat > "${gate_dir}/replicated_seed_harness.cpp" <<'CPP'
#include "replicated_seed.cpp"
int main() {
  pyc::gen::top sim;
  sim.a = pyc::cpp::Wire<8>(2);
  sim.b = pyc::cpp::Wire<8>(3);
  sim.c = pyc::cpp::Wire<8>(0x0f);
  sim.eval();
  if (sim.y.value() != 243 || sim.z.value() != 230 ||
      sim.dummy.value() != 0xf0) return 1;
  sim.a = pyc::cpp::Wire<8>(4);
  sim.eval();
  if (sim.y.value() != 251 || sim.z.value() != 236) return 2;
  sim.b = pyc::cpp::Wire<8>(5);
  sim.eval();
  return sim.y.value() == 245 && sim.z.value() == 232 ? 0 : 3;
}
CPP
sed 's/"replicated_seed.cpp"/"replicated_seed_off.cpp"/' \
  "${gate_dir}/replicated_seed_harness.cpp" > "${gate_dir}/replicated_seed_off_harness.cpp"
for variant in replicated_seed replicated_seed_off; do
  "${CXX:-c++}" -std=c++17 -I "${PYC_TOOLCHAIN_ROOT}/include" -I "${gate_dir}" \
    "${gate_dir}/${variant}_harness.cpp" -o "${gate_dir}/${variant}_harness"
  "${gate_dir}/${variant}_harness"
done

# A replicated singleton may read upstream expressions that stay materialized.
"${PYCC}" "${repo_root}/tests/newcircuit/replicated_producer.pyc" \
  --emit=cpp --sim-supernode-max-size=0 -o "${gate_dir}/replicated_producer.cpp"
"${PYCC}" "${repo_root}/tests/newcircuit/replicated_producer.pyc" \
  --emit=cpp --sim-supernode-max-size=0 --sim-replication=false \
  -o "${gate_dir}/replicated_producer_off.cpp"
if rg -q '^[[:space:]]+pyc_xor_7 = ' "${gate_dir}/replicated_producer.cpp"; then
  echo 'producer-dependent singleton still has a standalone assignment' >&2
  exit 1
fi
rg -q '^[[:space:]]+pyc_xor_7 = ' "${gate_dir}/replicated_producer_off.cpp"
rg -q 'pyc_and_1 \^ pyc_or_4' "${gate_dir}/replicated_producer.cpp"
cat > "${gate_dir}/replicated_producer_harness.cpp" <<'CPP'
#include "replicated_producer.cpp"
#include <cstdint>
int main() {
  pyc::gen::top sim;
  std::uint32_t random = 0x5167e2a3u;
  auto next = [&]() {
    random ^= random << 13;
    random ^= random >> 17;
    random ^= random << 5;
    return random;
  };
  for (unsigned i = 0; i < 10000; ++i) {
    unsigned a = next() & 255u, b = next() & 255u, c = next() & 255u;
    sim.a = pyc::cpp::Wire<8>(a);
    sim.b = pyc::cpp::Wire<8>(b);
    sim.c = pyc::cpp::Wire<8>(c);
    sim.eval();
    unsigned left = a & b, right = b | c, seed = left ^ right;
    if (sim.left.value() != left || sim.right.value() != right ||
        sim.y.value() != ((seed + (c << 1) + a) & 255u) ||
        sim.z.value() != ((seed - 2 * c - b) & 255u) ||
        sim.dummy.value() != ((~c) & 255u) ||
        sim.other.value() != (c >> 1))
      return 1;
  }
  return 0;
}
CPP
sed 's/"replicated_producer.cpp"/"replicated_producer_off.cpp"/' \
  "${gate_dir}/replicated_producer_harness.cpp" > "${gate_dir}/replicated_producer_off_harness.cpp"
for variant in replicated_producer replicated_producer_off; do
  "${CXX:-c++}" -std=c++17 -I "${PYC_TOOLCHAIN_ROOT}/include" -I "${gate_dir}" \
    "${gate_dir}/${variant}_harness.cpp" -o "${gate_dir}/${variant}_harness"
  "${gate_dir}/${variant}_harness"
done

# GSIM's singleton cost rule also applies to scalar arithmetic expressions.
sed 's/%seed = pyc.xor/%seed = pyc.add/' \
  "${repo_root}/tests/newcircuit/replicated_seed.pyc" > "${gate_dir}/replicated_add.pyc"
"${PYCC}" "${gate_dir}/replicated_add.pyc" --emit=cpp \
  --sim-supernode-max-size=0 -o "${gate_dir}/replicated_add.cpp"
"${PYCC}" "${gate_dir}/replicated_add.pyc" --emit=cpp \
  --sim-supernode-max-size=0 --sim-replication=false \
  -o "${gate_dir}/replicated_add_off.cpp"
if rg -q '^[[:space:]]+pyc_add_1 = ' "${gate_dir}/replicated_add.cpp"; then
  echo 'arithmetic singleton still has a standalone assignment' >&2
  exit 1
fi
rg -q '^[[:space:]]+pyc_add_1 = ' "${gate_dir}/replicated_add_off.cpp"
cat > "${gate_dir}/replicated_add_harness.cpp" <<'CPP'
#include "replicated_add.cpp"
#include <cstdint>
int main() {
  pyc::gen::top sim;
  std::uint32_t random = 0x6324a5d1u;
  for (unsigned i = 0; i < 10000; ++i) {
    random ^= random << 13;
    random ^= random >> 17;
    random ^= random << 5;
    unsigned a = random & 255u, b = (random >> 8) & 255u;
    unsigned c = (random >> 16) & 255u;
    sim.a = pyc::cpp::Wire<8>(a);
    sim.b = pyc::cpp::Wire<8>(b);
    sim.c = pyc::cpp::Wire<8>(c);
    sim.eval();
    unsigned seed = (a + b) & 255u;
    if (sim.y.value() != ((seed + ((~c) & 255u) + a) & 255u) ||
        sim.z.value() != ((seed - 2 * c + b) & 255u) ||
        sim.dummy.value() != ((~c) & 255u)) return 1;
  }
  return 0;
}
CPP
sed 's/"replicated_add.cpp"/"replicated_add_off.cpp"/' \
  "${gate_dir}/replicated_add_harness.cpp" > "${gate_dir}/replicated_add_off_harness.cpp"
for variant in replicated_add replicated_add_off; do
  "${CXX:-c++}" -std=c++17 -I "${PYC_TOOLCHAIN_ROOT}/include" -I "${gate_dir}" \
    "${gate_dir}/${variant}_harness.cpp" -o "${gate_dir}/${variant}_harness"
  "${gate_dir}/${variant}_harness"
done

# A one-use source and two uses in one target group each need one copy of the
# source expression. The original graph node should disappear after copying.
cat > "${gate_dir}/replication_group_harness.cpp" <<'CPP'
#include "replication_under_test.cpp"
int main() {
  pyc::gen::top sim;
  for (unsigned step = 0; step < 256; ++step) {
    unsigned a = (step * 17u) & 255u;
    unsigned b = (step * 53u + 7u) & 255u;
    unsigned c = (step * 29u + 11u) & 255u;
    sim.a = pyc::cpp::Wire<8>(a);
    sim.b = pyc::cpp::Wire<8>(b);
    sim.c = pyc::cpp::Wire<8>(c);
    sim.eval();
    unsigned seed = a ^ b;
    unsigned barrier = (~c) & 255u;
#if REPLICATED_REPEAT
    unsigned expected = ((seed + seed) & 255u) ^ barrier;
#else
    unsigned expected = ((seed + barrier) & 255u) ^ c;
#endif
    if (sim.y.value() != expected) return 1;
  }
  return 0;
}
CPP
for fixture in replicated_one_use replicated_same_group; do
  for mode in on off; do
    args=()
    if [[ "${mode}" == off ]]; then args+=(--sim-replication=false); fi
    "${PYCC}" "${repo_root}/tests/newcircuit/${fixture}.pyc" --emit=cpp \
      --sim-supernode-max-size=1 "${args[@]}" \
      -o "${gate_dir}/${fixture}_${mode}.cpp"
    if [[ "${mode}" == on ]]; then
      ! rg -q '^[[:space:]]+pyc_xor_1 = ' "${gate_dir}/${fixture}_${mode}.cpp"
      rg -q 'inline void eval_sim_group_' "${gate_dir}/${fixture}_${mode}.cpp"
    else
      rg -q '^[[:space:]]+pyc_xor_1 = ' "${gate_dir}/${fixture}_${mode}.cpp"
    fi
    cp "${gate_dir}/${fixture}_${mode}.cpp" "${gate_dir}/replication_under_test.cpp"
    repeat=0
    if [[ "${fixture}" == replicated_same_group ]]; then repeat=1; fi
    "${CXX:-c++}" -std=c++17 -I "${PYC_TOOLCHAIN_ROOT}/include" -I "${gate_dir}" \
      -DREPLICATED_REPEAT="${repeat}" "${gate_dir}/replication_group_harness.cpp" \
      -o "${gate_dir}/${fixture}_${mode}_harness"
    "${gate_dir}/${fixture}_${mode}_harness"
  done
done

# A copied source can itself contain a previously copied expression. The
# bounded recursive cost permits this two-operation scalar fragment.
cat > "${gate_dir}/replication_chain_harness.cpp" <<'CPP'
#include "replication_under_test.cpp"
int main() {
  pyc::gen::top sim;
  for (unsigned step = 0; step < 256; ++step) {
    unsigned a = (step * 17u) & 255u;
    unsigned b = (step * 53u + 7u) & 255u;
    unsigned c = (step * 29u + 11u) & 255u;
    unsigned d = (step * 83u + 3u) & 255u;
    sim.a = pyc::cpp::Wire<8>(a);
    sim.b = pyc::cpp::Wire<8>(b);
    sim.c = pyc::cpp::Wire<8>(c);
    sim.d = pyc::cpp::Wire<8>(d);
    sim.eval();
    unsigned expected = (((a ^ b) + c) & 255u) ^ ((~d) & 255u);
    if (sim.y.value() != expected) return 1;
  }
  return 0;
}
CPP
for mode in on off; do
  args=()
  if [[ "${mode}" == off ]]; then args+=(--sim-replication=false); fi
  "${PYCC}" "${repo_root}/tests/newcircuit/replicated_chain.pyc" --emit=cpp \
    --sim-supernode-max-size=1 "${args[@]}" \
    -o "${gate_dir}/replicated_chain_${mode}.cpp"
  if [[ "${mode}" == on ]]; then
    ! rg -q '^[[:space:]]+pyc_xor_1 = ' "${gate_dir}/replicated_chain_${mode}.cpp"
    ! rg -q '^[[:space:]]+pyc_add_2 = ' "${gate_dir}/replicated_chain_${mode}.cpp"
  else
    rg -q '^[[:space:]]+pyc_xor_1 = ' "${gate_dir}/replicated_chain_${mode}.cpp"
    rg -q '^[[:space:]]+pyc_add_2 = ' "${gate_dir}/replicated_chain_${mode}.cpp"
  fi
  cp "${gate_dir}/replicated_chain_${mode}.cpp" "${gate_dir}/replication_under_test.cpp"
  "${CXX:-c++}" -std=c++17 -I "${PYC_TOOLCHAIN_ROOT}/include" -I "${gate_dir}" \
    "${gate_dir}/replication_chain_harness.cpp" \
    -o "${gate_dir}/replicated_chain_${mode}_harness"
  "${gate_dir}/replicated_chain_${mode}_harness"
done

# GSIM's scalar array read cost permits copying a fixed-index vector read
# into a consumer group while the vector source remains materialized.
cat > "${gate_dir}/replication_vector_read_harness.cpp" <<'CPP'
#include "replication_under_test.cpp"
int main() {
  pyc::gen::top sim;
  for (unsigned step = 0; step < 256; ++step) {
    unsigned a = (step * 17u) & 255u;
    unsigned b = (step * 53u + 7u) & 255u;
    unsigned c = (step * 29u + 11u) & 255u;
    sim.v[0] = pyc::cpp::Wire<8>(a);
    sim.v[1] = pyc::cpp::Wire<8>(b);
    sim.c = pyc::cpp::Wire<8>(c);
    sim.eval();
    unsigned expected = (((a + ((~c) & 255u)) & 255u) ^ c);
    if (sim.y.value() != expected) return 1;
  }
  return 0;
}
CPP
for mode in on off; do
  args=()
  if [[ "${mode}" == off ]]; then args+=(--sim-replication=false); fi
  "${PYCC}" "${repo_root}/tests/newcircuit/replicated_vector_read.pyc" \
    --emit=cpp --sim-supernode-max-size=1 "${args[@]}" \
    -o "${gate_dir}/replicated_vector_read_${mode}.cpp"
  if [[ "${mode}" == on ]]; then
    ! rg -q '^[[:space:]]+pyc_v_get_1 = ' \
      "${gate_dir}/replicated_vector_read_${mode}.cpp"
    rg -F -q 'v[0]' "${gate_dir}/replicated_vector_read_${mode}.cpp"
  else
    rg -q '^[[:space:]]+pyc_v_get_1 = ' \
      "${gate_dir}/replicated_vector_read_${mode}.cpp"
  fi
  cp "${gate_dir}/replicated_vector_read_${mode}.cpp" \
    "${gate_dir}/replication_under_test.cpp"
  "${CXX:-c++}" -std=c++17 -I "${PYC_TOOLCHAIN_ROOT}/include" -I "${gate_dir}" \
    "${gate_dir}/replication_vector_read_harness.cpp" \
    -o "${gate_dir}/replicated_vector_read_${mode}_harness"
  "${gate_dir}/replicated_vector_read_${mode}_harness"
done

# A long pure chain must be partitioned into bounded supernodes while keeping
# the same value. Compare the generated number of groups with partitioning off.
python3 - "${repo_root}/tests/newcircuit/group_activation.pyc" \
  "${gate_dir}/supernode.pyc" <<'PY'
from pathlib import Path
import sys
source = Path(sys.argv[1]).read_text(encoding="utf-8")
header = source.split("    %t = pyc.add", 1)[0]
ops = []
for i in range(40):
    left = "%a" if i == 0 else f"%n{i - 1}"
    ops.append(f"    %n{i} = pyc.add {left}, %b : i8, i8 -> i8\n")
Path(sys.argv[2]).write_text(
    header + "".join(ops) + "    return %n39 : i8\n  }\n}\n",
    encoding="utf-8",
)
PY
"${PYCC}" "${gate_dir}/supernode.pyc" --emit=cpp \
  --sim-supernode-max-size=8 --logic-depth=256 -o "${gate_dir}/supernode.cpp"
"${PYCC}" "${gate_dir}/supernode.pyc" --emit=cpp \
  --sim-supernode-max-size=0 --logic-depth=256 -o "${gate_dir}/supernode_off.cpp"
test "$(rg -c 'inline void eval_sim_group_[0-9]+\(' "${gate_dir}/supernode.cpp")" = 5
test "$(rg -c 'inline void eval_sim_group_[0-9]+\(' "${gate_dir}/supernode_off.cpp")" = 1
cat > "${gate_dir}/supernode_harness.cpp" <<'CPP'
#include <sstream>
#include <string>
#include "supernode.cpp"
int main() {
  pyc::gen::top sim;
  sim.a = pyc::cpp::Wire<8>(3);
  sim.b = pyc::cpp::Wire<8>(2);
  sim.eval();
  if (sim.y.value() != 83) return 1;
  sim.eval();
  std::ostringstream stats;
  sim.dump_sim_stats(stats);
  if (stats.str().find("group_eval_calls=5\n") == std::string::npos) return 2;
  if (stats.str().find("group_cache_skips=5\n") == std::string::npos) return 3;
  return 0;
}
CPP
"${CXX:-c++}" -std=c++17 -I "${PYC_TOOLCHAIN_ROOT}/include" -I "${gate_dir}" \
  "${gate_dir}/supernode_harness.cpp" -o "${gate_dir}/supernode_harness"
PYC_SIM_STATS=1 "${gate_dir}/supernode_harness"

"${PYCC}" "${repo_root}/tests/newcircuit/bit_slice_split.pyc" \
  --emit=cpp --dump-pass-ir="${gate_dir}/slice_ir" \
  --dump-pass-ir-filter=comb-canonicalize -o "${gate_dir}/bit_slice.cpp"
rg -q 'pyc.concat' "${gate_dir}/slice_ir"/*before*.mlir
if rg -q 'pyc.concat|pyc.extract' "${gate_dir}/slice_ir"/*after*.mlir; then
  echo 'bit slice was not pushed through the pure expression' >&2
  exit 1
fi
rg -q 'pyc.not %arg1 : i8' "${gate_dir}/slice_ir"/*after*.mlir -F
cat > "${gate_dir}/bit_slice_harness.cpp" <<'CPP'
#include "bit_slice.cpp"
int main() {
  pyc::gen::top sim;
  sim.a = pyc::cpp::Wire<8>(0x55);
  sim.b = pyc::cpp::Wire<8>(0x12);
  sim.eval();
  if (sim.y.value() != 0xed) return 1;
  sim.a = pyc::cpp::Wire<8>(0xaa);
  sim.eval();
  return sim.y.value() == 0xed ? 0 : 2;
}
CPP
"${CXX:-c++}" -std=c++17 -I "${PYC_TOOLCHAIN_ROOT}/include" -I "${gate_dir}" \
  "${gate_dir}/bit_slice_harness.cpp" -o "${gate_dir}/bit_slice_harness"
"${gate_dir}/bit_slice_harness"
if command -v verilator >/dev/null 2>&1; then
  "${PYCC}" "${repo_root}/tests/newcircuit/bit_slice_split.pyc" \
    --emit=verilog -o "${gate_dir}/bit_slice.v"
  cat > "${gate_dir}/bit_slice_tb.sv" <<'SV'
module bit_slice_tb;
  reg [7:0] a, b;
  wire [7:0] y;
  top dut(.a(a), .b(b), .y(y));
  initial begin
    a = 8'h55; b = 8'h12; #1; if (y !== 8'hed) $fatal;
    a = 8'haa; #1; if (y !== 8'hed) $fatal;
    $finish;
  end
endmodule
SV
  verilator --binary --timing --top-module bit_slice_tb \
    -I"${repo_root}/runtime/verilog" --Mdir "${gate_dir}/bit_slice_vobj" \
    "${gate_dir}/bit_slice.v" "${gate_dir}/bit_slice_tb.sv" >/dev/null
  "${gate_dir}/bit_slice_vobj/Vbit_slice_tb" >/dev/null
fi

# A slice spanning several concatenation fields is rebuilt from the exact
# overlapping pieces in high-to-low order.
"${PYCC}" "${repo_root}/tests/newcircuit/cross_field_slice.pyc" \
  --emit=cpp --dump-pass-ir="${gate_dir}/cross_field_ir" \
  --dump-pass-ir-filter=comb-canonicalize -o "${gate_dir}/cross_field.cpp"
rg -q 'pyc.concat.*i4, i8, i4.*-> i16' \
  "${gate_dir}/cross_field_ir"/*after*.mlir
cat > "${gate_dir}/cross_field_harness.cpp" <<'CPP'
#include "cross_field.cpp"
int main() {
  pyc::gen::top sim;
  sim.a = pyc::cpp::Wire<8>(0xab);
  sim.b = pyc::cpp::Wire<8>(0xcd);
  sim.c = pyc::cpp::Wire<8>(0xef);
  sim.eval();
  if (sim.y.value() != 0xbcde) return 1;
  sim.a = pyc::cpp::Wire<8>(0x12);
  sim.b = pyc::cpp::Wire<8>(0x34);
  sim.c = pyc::cpp::Wire<8>(0x56);
  sim.eval();
  return sim.y.value() == 0x2345 ? 0 : 2;
}
CPP
"${CXX:-c++}" -std=c++17 -I "${PYC_TOOLCHAIN_ROOT}/include" -I "${gate_dir}" \
  "${gate_dir}/cross_field_harness.cpp" -o "${gate_dir}/cross_field_harness"
"${gate_dir}/cross_field_harness"
if command -v verilator >/dev/null 2>&1; then
  "${PYCC}" "${repo_root}/tests/newcircuit/cross_field_slice.pyc" \
    --emit=verilog -o "${gate_dir}/cross_field.v"
  cat > "${gate_dir}/cross_field_tb.sv" <<'SV'
module cross_field_tb;
  reg [7:0] a, b, c;
  wire [15:0] y;
  top dut(.a(a), .b(b), .c(c), .y(y));
  initial begin
    a = 8'hab; b = 8'hcd; c = 8'hef; #1; if (y !== 16'hbcde) $fatal;
    a = 8'h12; b = 8'h34; c = 8'h56; #1; if (y !== 16'h2345) $fatal;
    $finish;
  end
endmodule
SV
  verilator --binary --timing --top-module cross_field_tb \
    -I"${repo_root}/runtime/verilog" --Mdir "${gate_dir}/cross_field_vobj" \
    "${gate_dir}/cross_field.v" "${gate_dir}/cross_field_tb.sv" >/dev/null
  "${gate_dir}/cross_field_vobj/Vcross_field_tb" >/dev/null
fi

# An observed slice of add needs only operand bits through the top requested
# bit. Verify the hardware rewrite and the shared C++/Verilog behavior.
"${PYCC}" "${repo_root}/tests/newcircuit/arithmetic_slice.pyc" \
  --emit=cpp --dump-pass-ir="${gate_dir}/arithmetic_slice_ir" \
  --dump-pass-ir-filter=comb-canonicalize -o "${gate_dir}/arithmetic_slice.cpp"
rg -q 'pyc.add .* : i16, i16 -> i16' \
  "${gate_dir}/arithmetic_slice_ir"/*before*.mlir
rg -q 'pyc.add .* : i8, i8 -> i8' \
  "${gate_dir}/arithmetic_slice_ir"/*after*.mlir
cat > "${gate_dir}/arithmetic_slice_harness.cpp" <<'CPP'
#include "arithmetic_slice.cpp"
int main() {
  pyc::gen::top sim;
  sim.a = pyc::cpp::Wire<16>(0x000f);
  sim.b = pyc::cpp::Wire<16>(0x0001);
  sim.eval();
  if (sim.y.value() != 1) return 1;
  sim.a = pyc::cpp::Wire<16>(0x00ff);
  sim.eval();
  if (sim.y.value() != 0) return 2;
  sim.a = pyc::cpp::Wire<16>(0x0050);
  sim.b = pyc::cpp::Wire<16>(0x0020);
  sim.eval();
  return sim.y.value() == 7 ? 0 : 3;
}
CPP
"${CXX:-c++}" -std=c++17 -I "${PYC_TOOLCHAIN_ROOT}/include" -I "${gate_dir}" \
  "${gate_dir}/arithmetic_slice_harness.cpp" -o "${gate_dir}/arithmetic_slice_harness"
"${gate_dir}/arithmetic_slice_harness"
if command -v verilator >/dev/null 2>&1; then
  "${PYCC}" "${repo_root}/tests/newcircuit/arithmetic_slice.pyc" \
    --emit=verilog -o "${gate_dir}/arithmetic_slice.v"
  cat > "${gate_dir}/arithmetic_slice_tb.sv" <<'SV'
module arithmetic_slice_tb;
  reg [15:0] a, b;
  wire [3:0] y;
  top dut(.a(a), .b(b), .y(y));
  initial begin
    a = 16'h000f; b = 16'h0001; #1; if (y !== 4'h1) $fatal;
    a = 16'h00ff; #1; if (y !== 4'h0) $fatal;
    a = 16'h0050; b = 16'h0020; #1; if (y !== 4'h7) $fatal;
    $finish;
  end
endmodule
SV
  verilator --binary --timing --top-module arithmetic_slice_tb \
    -I"${repo_root}/runtime/verilog" --Mdir "${gate_dir}/arithmetic_slice_vobj" \
    "${gate_dir}/arithmetic_slice.v" "${gate_dir}/arithmetic_slice_tb.sv" >/dev/null
  "${gate_dir}/arithmetic_slice_vobj/Varithmetic_slice_tb" >/dev/null
fi

# Multiple slices of one modular result share a single operation narrowed to
# the highest observed bit.
"${PYCC}" "${repo_root}/tests/newcircuit/multi_reader_arithmetic.pyc" \
  --emit=cpp --dump-pass-ir="${gate_dir}/multi_reader_arithmetic_ir" \
  --dump-pass-ir-filter=comb-canonicalize \
  -o "${gate_dir}/multi_reader_arithmetic.cpp"
for op in add sub mul; do
  rg -q "pyc.${op} .* : i40, i40 -> i40" \
    "${gate_dir}/multi_reader_arithmetic_ir"/*after*.mlir
  ! rg -q "pyc.${op} .* : i128, i128 -> i128" \
    "${gate_dir}/multi_reader_arithmetic_ir"/*after*.mlir
done
"${CXX:-c++}" -std=c++17 -I "${PYC_TOOLCHAIN_ROOT}/include" \
  -I "${gate_dir}" "${repo_root}/tests/newcircuit/multi_reader_arithmetic_harness.cpp" \
  -o "${gate_dir}/multi_reader_arithmetic_harness"
"${gate_dir}/multi_reader_arithmetic_harness"
if command -v iverilog >/dev/null 2>&1; then
  "${PYCC}" "${repo_root}/tests/newcircuit/multi_reader_arithmetic.pyc" \
    --emit=verilog -o "${gate_dir}/multi_reader_arithmetic.v"
  iverilog -g2012 -s multi_reader_arithmetic_tb \
    -I"${repo_root}/runtime/verilog" -o "${gate_dir}/multi_reader_arithmetic_tb" \
    "${gate_dir}/multi_reader_arithmetic.v" \
    "${repo_root}/tests/newcircuit/multi_reader_arithmetic_tb.sv"
  "${gate_dir}/multi_reader_arithmetic_tb"
fi

# Multiple slices of one mux result share a single narrowed selection.
"${PYCC}" "${repo_root}/tests/newcircuit/multi_reader_mux.pyc" \
  --emit=cpp --dump-pass-ir="${gate_dir}/multi_reader_mux_ir" \
  --dump-pass-ir-filter=comb-canonicalize \
  -o "${gate_dir}/multi_reader_mux.cpp"
rg -q 'pyc.mux .* : i1, i40, i40 -> i40' \
  "${gate_dir}/multi_reader_mux_ir"/*after*.mlir
! rg -q 'pyc.mux .* : i1, i128, i128 -> i128' \
  "${gate_dir}/multi_reader_mux_ir"/*after*.mlir
"${CXX:-c++}" -std=c++17 -I "${PYC_TOOLCHAIN_ROOT}/include" \
  -I "${gate_dir}" "${repo_root}/tests/newcircuit/multi_reader_mux_harness.cpp" \
  -o "${gate_dir}/multi_reader_mux_harness"
"${gate_dir}/multi_reader_mux_harness"
if command -v iverilog >/dev/null 2>&1; then
  "${PYCC}" "${repo_root}/tests/newcircuit/multi_reader_mux.pyc" \
    --emit=verilog -o "${gate_dir}/multi_reader_mux.v"
  iverilog -g2012 -s multi_reader_mux_tb \
    -I"${repo_root}/runtime/verilog" -o "${gate_dir}/multi_reader_mux_tb" \
    "${gate_dir}/multi_reader_mux.v" \
    "${repo_root}/tests/newcircuit/multi_reader_mux_tb.sv"
  "${gate_dir}/multi_reader_mux_tb"
fi

# Shared fixed shifts retain only the highest demanded output bit. Constant
# dynamic shifts join the same path; right-shift fill is explicit extension.
"${PYCC}" "${repo_root}/tests/newcircuit/multi_reader_immediate_shift.pyc" \
  --emit=cpp --dump-pass-ir="${gate_dir}/multi_reader_immediate_shift_ir" \
  --dump-pass-ir-filter=comb-canonicalize \
  -o "${gate_dir}/multi_reader_immediate_shift.cpp"
rg -q 'pyc.shli .* : i40' \
  "${gate_dir}/multi_reader_immediate_shift_ir"/*after*.mlir
! rg -q 'pyc.shli .* : i128' \
  "${gate_dir}/multi_reader_immediate_shift_ir"/*after*.mlir
! rg -q 'pyc.lshri .* : i128' \
  "${gate_dir}/multi_reader_immediate_shift_ir"/*after*.mlir
! rg -q 'pyc.ashri .* : i128' \
  "${gate_dir}/multi_reader_immediate_shift_ir"/*after*.mlir
! rg -q 'pyc.(shl|lshr|ashr) ' \
  "${gate_dir}/multi_reader_immediate_shift_ir"/*after*.mlir
rg -q 'pyc.sext .* : i1 -> i8' \
  "${gate_dir}/multi_reader_immediate_shift_ir"/*after*.mlir
"${CXX:-c++}" -std=c++17 -I "${PYC_TOOLCHAIN_ROOT}/include" \
  -I "${gate_dir}" "${repo_root}/tests/newcircuit/multi_reader_immediate_shift_harness.cpp" \
  -o "${gate_dir}/multi_reader_immediate_shift_harness"
"${gate_dir}/multi_reader_immediate_shift_harness"
if command -v iverilog >/dev/null 2>&1; then
  "${PYCC}" "${repo_root}/tests/newcircuit/multi_reader_immediate_shift.pyc" \
    --emit=verilog -o "${gate_dir}/multi_reader_immediate_shift.v"
  iverilog -g2012 -s multi_reader_immediate_shift_tb \
    -I"${repo_root}/runtime/verilog" -o "${gate_dir}/multi_reader_immediate_shift_tb" \
    "${gate_dir}/multi_reader_immediate_shift.v" \
    "${repo_root}/tests/newcircuit/multi_reader_immediate_shift_tb.sv"
  "${gate_dir}/multi_reader_immediate_shift_tb"
fi

# Multiple non-overlapping consumers can split one wide bitwise value into
# independent narrow hardware operations.
"${PYCC}" "${repo_root}/tests/newcircuit/multi_slice_bitwise.pyc" \
  --emit=cpp --dump-pass-ir="${gate_dir}/multi_slice_ir" \
  --dump-pass-ir-filter=comb-canonicalize -o "${gate_dir}/multi_slice.cpp"
test "$(rg -c 'pyc.and .* : i8, i8 -> i8' "${gate_dir}/multi_slice_ir"/*after*.mlir)" = 2
if rg -q 'pyc.and .* : i16, i16 -> i16' \
    "${gate_dir}/multi_slice_ir"/*after*.mlir; then
  echo 'wide bitwise operation was not split across its slice users' >&2
  exit 1
fi
cat > "${gate_dir}/multi_slice_harness.cpp" <<'CPP'
#include "multi_slice.cpp"
int main() {
  pyc::gen::top sim;
  sim.a = pyc::cpp::Wire<16>(0x12f0);
  sim.b = pyc::cpp::Wire<16>(0x0ff0);
  sim.eval();
  if (sim.lo.value() != 0xf0 || sim.hi.value() != 0x02) return 1;
  sim.a = pyc::cpp::Wire<16>(0x3456);
  sim.b = pyc::cpp::Wire<16>(0xffff);
  sim.eval();
  return sim.lo.value() == 0x56 && sim.hi.value() == 0x34 ? 0 : 2;
}
CPP
"${CXX:-c++}" -std=c++17 -I "${PYC_TOOLCHAIN_ROOT}/include" -I "${gate_dir}" \
  "${gate_dir}/multi_slice_harness.cpp" -o "${gate_dir}/multi_slice_harness"
"${gate_dir}/multi_slice_harness"
if command -v verilator >/dev/null 2>&1; then
  "${PYCC}" "${repo_root}/tests/newcircuit/multi_slice_bitwise.pyc" \
    --emit=verilog -o "${gate_dir}/multi_slice.v"
  cat > "${gate_dir}/multi_slice_tb.sv" <<'SV'
module multi_slice_tb;
  reg [15:0] a, b;
  wire [7:0] lo, hi;
  top dut(.a(a), .b(b), .lo(lo), .hi(hi));
  initial begin
    a = 16'h12f0; b = 16'h0ff0; #1;
    if (lo !== 8'hf0 || hi !== 8'h02) $fatal;
    a = 16'h3456; b = 16'hffff; #1;
    if (lo !== 8'h56 || hi !== 8'h34) $fatal;
    $finish;
  end
endmodule
SV
  verilator --binary --timing --top-module multi_slice_tb \
    -I"${repo_root}/runtime/verilog" --Mdir "${gate_dir}/multi_slice_vobj" \
    "${gate_dir}/multi_slice.v" "${gate_dir}/multi_slice_tb.sv" >/dev/null
  "${gate_dir}/multi_slice_vobj/Vmulti_slice_tb" >/dev/null
fi

# More than 64 observed slices still split the hardware value graph. The
# sweep over slice boundaries avoids a quadratic reader scan.
python3 "${repo_root}/tests/newcircuit/generate_many_slices.py" "${gate_dir}"
"${PYCC}" "${gate_dir}/many_vector_lanes.pyc" \
  --emit=cpp --dump-pass-ir="${gate_dir}/many_vector_lanes_ir" \
  --dump-pass-ir-filter=comb-canonicalize \
  -o "${gate_dir}/many_vector_lanes.cpp"
test "$(rg -c 'pyc.udiv .* : i8, i8 -> i8' \
  "${gate_dir}/many_vector_lanes_ir"/*after*.mlir)" = 65
! rg -q 'pyc.udiv .* : vector<65xi8>' \
  "${gate_dir}/many_vector_lanes_ir"/*after*.mlir
cat > "${gate_dir}/many_vector_lanes_harness.cpp" <<'CPP'
#include "many_vector_lanes.cpp"
int main() {
  pyc::gen::top sim;
  for (unsigned step = 0; step < 32; ++step) {
    for (unsigned lane = 0; lane < 65; ++lane) {
      sim.a[lane] = pyc::cpp::Wire<8>((step * 11 + lane * 19) & 255);
      sim.b[lane] = pyc::cpp::Wire<8>(1 + (step * 3 + lane * 7) % 31);
    }
    sim.eval();
    for (unsigned lane = 0; lane < 65; ++lane) {
      unsigned a = (step * 11 + lane * 19) & 255;
      unsigned b = 1 + (step * 3 + lane * 7) % 31;
      if (sim.y[lane].value() != (a / b < 3)) return 1;
    }
  }
  return 0;
}
CPP
"${CXX:-c++}" -std=c++17 -I "${PYC_TOOLCHAIN_ROOT}/include" -I "${gate_dir}" \
  "${gate_dir}/many_vector_lanes_harness.cpp" -o "${gate_dir}/many_vector_lanes_harness"
"${gate_dir}/many_vector_lanes_harness"
"${PYCC}" "${gate_dir}/many_vector_register.pyc" \
  --emit=cpp --dump-pass-ir="${gate_dir}/many_vector_register_ir" \
  --dump-pass-ir-filter=comb-canonicalize \
  -o "${gate_dir}/many_vector_register.cpp"
test "$(rg -c 'pyc.reg .* : i8' \
  "${gate_dir}/many_vector_register_ir"/*after*.mlir)" = 65
! rg -q 'pyc.reg .* : vector<65xi8>' \
  "${gate_dir}/many_vector_register_ir"/*after*.mlir
cat > "${gate_dir}/many_vector_register_harness.cpp" <<'CPP'
#include "many_vector_register.cpp"
int main() {
  pyc::gen::top sim;
  sim.clk = pyc::cpp::Wire<1>(0);
  sim.rst = pyc::cpp::Wire<1>(1);
  auto pulse = [&]() {
    sim.clk = pyc::cpp::Wire<1>(0); sim.step();
    sim.clk = pyc::cpp::Wire<1>(1); sim.step();
  };
  for (unsigned lane = 0; lane < 65; ++lane)
    sim.d[lane] = pyc::cpp::Wire<8>(lane);
  pulse();
  for (unsigned lane = 0; lane < 65; ++lane)
    if (sim.y[lane].value() != 0) return 1;
  sim.rst = pyc::cpp::Wire<1>(0);
  for (unsigned step = 0; step < 16; ++step) {
    for (unsigned lane = 0; lane < 65; ++lane)
      sim.d[lane] = pyc::cpp::Wire<8>((step * 13 + lane * 17) & 255);
    pulse();
    for (unsigned lane = 0; lane < 65; ++lane)
      if (sim.y[64 - lane].value() != ((step * 13 + lane * 17) & 255))
        return 2;
  }
  return 0;
}
CPP
"${CXX:-c++}" -std=c++17 -I "${PYC_TOOLCHAIN_ROOT}/include" -I "${gate_dir}" \
  "${gate_dir}/many_vector_register_harness.cpp" -o "${gate_dir}/many_vector_register_harness"
"${gate_dir}/many_vector_register_harness"
"${PYCC}" "${repo_root}/tests/newcircuit/split_vector_extended.pyc" \
  --emit=cpp --dump-pass-ir="${gate_dir}/split_vector_extended_ir" \
  --dump-pass-ir-filter=comb-canonicalize \
  -o "${gate_dir}/split_vector_extended.cpp"
! rg -q 'pyc\.(eq|trunc|sext|extract|shl|ashri|sdiv).*vector<4x' \
  "${gate_dir}/split_vector_extended_ir"/*after*.mlir
cat > "${gate_dir}/split_vector_extended_harness.cpp" <<'CPP'
#include <cstdint>
#include "split_vector_extended.cpp"
int main() {
  pyc::gen::top sim;
  for (unsigned step = 0; step < 256; ++step) {
    for (unsigned lane = 0; lane < 4; ++lane) {
      sim.a[lane] = pyc::cpp::Wire<8>((step * 7 + lane * 13) & 255);
      sim.b[lane] = pyc::cpp::Wire<8>(1 + (step + lane * 3) % 31);
    }
    sim.amount = pyc::cpp::Wire<3>(step & 7);
    sim.eval();
    unsigned a = (step * 7 + 26) & 255;
    unsigned b = 1 + (step + 6) % 31;
    unsigned shift = step & 7;
    unsigned sign = (a & 8) ? 0xf0 : 0;
    unsigned arithmetic = (a >> 2) | ((a & 0x80) ? 0xc0 : 0);
    unsigned signedQuotient = static_cast<std::uint8_t>(
        static_cast<std::int8_t>(a) / static_cast<int>(b));
    if (sim.equal.value() != (a == b) ||
        sim.truncated.value() != (a & 15) ||
        sim.extended.value() != ((a & 15) | sign) ||
        sim.extracted.value() != ((a >> 2) & 15) ||
        sim.shifted.value() != ((a << shift) & 255) ||
        sim.arithmetic.value() != arithmetic ||
        sim.divided.value() != signedQuotient) return 1;
  }
  return 0;
}
CPP
"${CXX:-c++}" -std=c++17 -I "${PYC_TOOLCHAIN_ROOT}/include" -I "${gate_dir}" \
  "${gate_dir}/split_vector_extended_harness.cpp" -o "${gate_dir}/split_vector_extended_harness"
"${gate_dir}/split_vector_extended_harness"
"${PYCC}" "${gate_dir}/many_bitwise_slices.pyc" \
  --emit=cpp --dump-pass-ir="${gate_dir}/many_bitwise_ir" \
  --dump-pass-ir-filter=comb-canonicalize \
  -o "${gate_dir}/many_bitwise.cpp"
test "$(rg -c 'pyc.and .* : i1, i1 -> i1' \
  "${gate_dir}/many_bitwise_ir"/*after*.mlir)" = 65
! rg -q 'pyc.and .* : i65, i65 -> i65' \
  "${gate_dir}/many_bitwise_ir"/*after*.mlir
cat > "${gate_dir}/many_bitwise_harness.cpp" <<'CPP'
#include <cstdint>
#include "many_bitwise.cpp"
int main() {
  pyc::gen::top sim;
  for (unsigned step = 0; step < 100; ++step) {
    std::uint64_t a = UINT64_C(0x9e3779b97f4a7c15) * (step + 1);
    std::uint64_t b = UINT64_C(0xd1b54a32d192ed03) * (step + 3);
    unsigned ah = step & 1u, bh = (step >> 1) & 1u;
    sim.a = pyc::cpp::Wire<65>({a, ah});
    sim.b = pyc::cpp::Wire<65>({b, bh});
    sim.eval();
    for (unsigned bit = 0; bit < 65; ++bit) {
      unsigned expected = bit == 64 ? (ah & bh)
                                    : ((a >> bit) & (b >> bit) & 1u);
      if (sim.y[bit].value() != expected) return 1;
    }
  }
  return 0;
}
CPP
"${CXX:-c++}" -std=c++17 -I "${PYC_TOOLCHAIN_ROOT}/include" -I "${gate_dir}" \
  "${gate_dir}/many_bitwise_harness.cpp" -o "${gate_dir}/many_bitwise_harness"
"${gate_dir}/many_bitwise_harness"

# Width-cast slice readers use source bits and extension fill directly in
# both backends, including slices that cross the original width.
"${PYCC}" "${repo_root}/tests/newcircuit/cast_slice_demand.pyc" \
  --emit=cpp --dump-pass-ir="${gate_dir}/cast_slice_demand_ir" \
  --dump-pass-ir-filter=comb-canonicalize \
  -o "${gate_dir}/cast_slice_demand.cpp"
! rg -q 'pyc.zext .* : i16 -> i32' \
  "${gate_dir}/cast_slice_demand_ir"/*after*.mlir
! rg -q 'pyc.sext .* : i16 -> i32' \
  "${gate_dir}/cast_slice_demand_ir"/*after*.mlir
! rg -q 'pyc.trunc .* : i16 -> i8' \
  "${gate_dir}/cast_slice_demand_ir"/*after*.mlir
"${CXX:-c++}" -std=c++17 -I "${PYC_TOOLCHAIN_ROOT}/include" -I "${gate_dir}" \
  "${repo_root}/tests/newcircuit/cast_slice_demand_harness.cpp" \
  -o "${gate_dir}/cast_slice_demand_harness"
"${gate_dir}/cast_slice_demand_harness"
if command -v iverilog >/dev/null 2>&1; then
  "${PYCC}" "${repo_root}/tests/newcircuit/cast_slice_demand.pyc" \
    --emit=verilog -o "${gate_dir}/cast_slice_demand.v"
  iverilog -g2012 -s cast_slice_demand_tb \
    -I"${repo_root}/runtime/verilog" -o "${gate_dir}/cast_slice_demand_tb" \
    "${gate_dir}/cast_slice_demand.v" \
    "${repo_root}/tests/newcircuit/cast_slice_demand_tb.sv"
  "${gate_dir}/cast_slice_demand_tb"
fi

# A scalar mux with separated output slices computes only observed segments;
# overlapping readers share the middle segment in both backends.
"${PYCC}" "${repo_root}/tests/newcircuit/gapped_mux_slices.pyc" \
  --emit=cpp --dump-pass-ir="${gate_dir}/gapped_mux_slices_ir" \
  --dump-pass-ir-filter=comb-canonicalize \
  -o "${gate_dir}/gapped_mux_slices.cpp"
test "$(rg -c 'pyc.mux .* : i1, i4, i4 -> i4' \
  "${gate_dir}/gapped_mux_slices_ir"/*after*.mlir)" = 4
! rg -q 'pyc.mux .* : i1, i24, i24 -> i24' \
  "${gate_dir}/gapped_mux_slices_ir"/*after*.mlir
"${CXX:-c++}" -std=c++17 -I "${PYC_TOOLCHAIN_ROOT}/include" -I "${gate_dir}" \
  "${repo_root}/tests/newcircuit/gapped_mux_slices_harness.cpp" \
  -o "${gate_dir}/gapped_mux_slices_harness"
"${gate_dir}/gapped_mux_slices_harness"
if command -v iverilog >/dev/null 2>&1; then
  "${PYCC}" "${repo_root}/tests/newcircuit/gapped_mux_slices.pyc" \
    --emit=verilog -o "${gate_dir}/gapped_mux_slices.v"
  iverilog -g2012 -s gapped_mux_slices_tb \
    -I"${repo_root}/runtime/verilog" -o "${gate_dir}/gapped_mux_slices_tb" \
    "${gate_dir}/gapped_mux_slices.v" \
    "${repo_root}/tests/newcircuit/gapped_mux_slices_tb.sv"
  "${gate_dir}/gapped_mux_slices_tb"
fi

# Overlapping slice readers share the middle bitwise segment instead of
# recomputing a wide value or duplicating the overlap.
"${PYCC}" "${repo_root}/tests/newcircuit/overlap_bitwise.pyc" \
  --emit=cpp --dump-pass-ir="${gate_dir}/overlap_bitwise_ir" \
  --dump-pass-ir-filter=comb-canonicalize -o "${gate_dir}/overlap_bitwise.cpp"
test "$(rg -c 'pyc.and .* : i4, i4 -> i4' \
  "${gate_dir}/overlap_bitwise_ir"/*after*.mlir)" = 3
test "$(rg -c 'pyc.concat' \
  "${gate_dir}/overlap_bitwise_ir"/*after*.mlir)" = 2
cat > "${gate_dir}/overlap_bitwise_harness.cpp" <<'CPP'
#include "overlap_bitwise.cpp"
int main() {
  pyc::gen::top sim;
  sim.a = pyc::cpp::Wire<16>(0x12f0);
  sim.b = pyc::cpp::Wire<16>(0x0ff0);
  sim.eval();
  if (sim.lo.value() != 0xf0 || sim.hi.value() != 0x2f) return 1;
  sim.a = pyc::cpp::Wire<16>(0x3456);
  sim.b = pyc::cpp::Wire<16>(0xffff);
  sim.eval();
  return sim.lo.value() == 0x56 && sim.hi.value() == 0x45 ? 0 : 2;
}
CPP
"${CXX:-c++}" -std=c++17 -I "${PYC_TOOLCHAIN_ROOT}/include" -I "${gate_dir}" \
  "${gate_dir}/overlap_bitwise_harness.cpp" -o "${gate_dir}/overlap_bitwise_harness"
"${gate_dir}/overlap_bitwise_harness"
if command -v iverilog >/dev/null 2>&1; then
  "${PYCC}" "${repo_root}/tests/newcircuit/overlap_bitwise.pyc" \
    --emit=verilog -o "${gate_dir}/overlap_bitwise.v"
  cat > "${gate_dir}/overlap_bitwise_tb.sv" <<'SV'
module overlap_bitwise_tb;
  reg [15:0] a, b;
  wire [7:0] lo, hi;
  top dut(.a(a), .b(b), .lo(lo), .hi(hi));
  initial begin
    a = 16'h12f0; b = 16'h0ff0; #1;
    if (lo !== 8'hf0 || hi !== 8'h2f) $fatal;
    a = 16'h3456; b = 16'hffff; #1;
    if (lo !== 8'h56 || hi !== 8'h45) $fatal;
    a = 16'h0x55; b = 16'hffff; #1;
    if (lo !== 8'h55 || hi !== 8'hx5) $fatal;
    $finish;
  end
endmodule
SV
  iverilog -g2012 -s overlap_bitwise_tb -I"${repo_root}/runtime/verilog" \
    -o "${gate_dir}/overlap_bitwise_tb" "${gate_dir}/overlap_bitwise.v" \
    "${gate_dir}/overlap_bitwise_tb.sv"
  "${gate_dir}/overlap_bitwise_tb"
fi
"${PYCC}" "${repo_root}/tests/newcircuit/overlap_not.pyc" \
  --emit=cpp --dump-pass-ir="${gate_dir}/overlap_not_ir" \
  --dump-pass-ir-filter=comb-canonicalize -o "${gate_dir}/overlap_not.cpp"
test "$(rg -c 'pyc.not .* : i4' \
  "${gate_dir}/overlap_not_ir"/*after*.mlir)" = 3
cat > "${gate_dir}/overlap_not_harness.cpp" <<'CPP'
#include "overlap_not.cpp"
int main() {
  pyc::gen::top sim;
  sim.a = pyc::cpp::Wire<16>(0x12f0);
  sim.eval();
  if (sim.lo.value() != 0x0f || sim.hi.value() != 0xd0) return 1;
  sim.a = pyc::cpp::Wire<16>(0x3456);
  sim.eval();
  return sim.lo.value() == 0xa9 && sim.hi.value() == 0xba ? 0 : 2;
}
CPP
"${CXX:-c++}" -std=c++17 -I "${PYC_TOOLCHAIN_ROOT}/include" -I "${gate_dir}" \
  "${gate_dir}/overlap_not_harness.cpp" -o "${gate_dir}/overlap_not_harness"
"${gate_dir}/overlap_not_harness"

"${PYCC}" "${repo_root}/tests/newcircuit/bit_slice_logic_mux.pyc" \
  --emit=cpp --dump-pass-ir="${gate_dir}/logic_slice_ir" \
  --dump-pass-ir-filter=comb-canonicalize -o "${gate_dir}/logic_slice.cpp"
rg -q 'pyc.and .* : i4, i4 -> i4' "${gate_dir}/logic_slice_ir"/*after*.mlir
rg -q 'pyc.mux .* : i1, i4, i4 -> i4' "${gate_dir}/logic_slice_ir"/*after*.mlir
cat > "${gate_dir}/logic_slice_harness.cpp" <<'CPP'
#include "logic_slice.cpp"
int main() {
  pyc::gen::top sim;
  sim.a = pyc::cpp::Wire<16>(0x0f00);
  sim.b = pyc::cpp::Wire<16>(0x0a00);
  sim.c = pyc::cpp::Wire<16>(0x0500);
  sim.sel = pyc::cpp::Wire<1>(1);
  sim.eval();
  if (sim.y.value() != 10) return 1;
  sim.sel = pyc::cpp::Wire<1>(0);
  sim.eval();
  return sim.y.value() == 5 ? 0 : 2;
}
CPP
"${CXX:-c++}" -std=c++17 -I "${PYC_TOOLCHAIN_ROOT}/include" -I "${gate_dir}" \
  "${gate_dir}/logic_slice_harness.cpp" -o "${gate_dir}/logic_slice_harness"
"${gate_dir}/logic_slice_harness"
if command -v verilator >/dev/null 2>&1; then
  "${PYCC}" "${repo_root}/tests/newcircuit/bit_slice_logic_mux.pyc" \
    --emit=verilog -o "${gate_dir}/logic_slice.v"
  cat > "${gate_dir}/logic_slice_tb.sv" <<'SV'
module logic_slice_tb;
  reg sel;
  reg [15:0] a, b, c;
  wire [3:0] y;
  top dut(.sel(sel), .a(a), .b(b), .c(c), .y(y));
  initial begin
    a = 16'h0f00; b = 16'h0a00; c = 16'h0500;
    sel = 1; #1; if (y !== 4'ha) $fatal;
    sel = 0; #1; if (y !== 4'h5) $fatal;
    $finish;
  end
endmodule
SV
  verilator --binary --timing --top-module logic_slice_tb \
    -I"${repo_root}/runtime/verilog" --Mdir "${gate_dir}/logic_slice_vobj" \
    "${gate_dir}/logic_slice.v" "${gate_dir}/logic_slice_tb.sv" >/dev/null
  "${gate_dir}/logic_slice_vobj/Vlogic_slice_tb" >/dev/null
fi

# Fixed-shift slice propagation changes the hardware value graph and must
# produce identical C++ and Verilog results, including sign-fill X bits.
"${PYCC}" "${repo_root}/tests/newcircuit/immediate_shift_slice.pyc" \
  --emit=cpp --dump-pass-ir="${gate_dir}/immediate_shift_ir" \
  --dump-pass-ir-filter=comb-canonicalize \
  -o "${gate_dir}/immediate_shift_slice.cpp"
rg -q 'pyc.shli' "${gate_dir}/immediate_shift_ir"/*before*.mlir
rg -q 'pyc.lshri' "${gate_dir}/immediate_shift_ir"/*before*.mlir
if rg -q 'pyc.shli|pyc.lshri|pyc.ashri' "${gate_dir}/immediate_shift_ir"/*after*.mlir; then
  echo 'fixed-shift slices were not pushed into the source value' >&2
  exit 1
fi
rg -q 'pyc.sext' "${gate_dir}/immediate_shift_ir"/*after*.mlir
cat > "${gate_dir}/immediate_shift_slice_harness.cpp" <<'CPP'
#include "immediate_shift_slice.cpp"
int main() {
  pyc::gen::top sim;
  auto check = [&](unsigned a) {
    sim.a = pyc::cpp::Wire<16>(a);
    sim.eval();
    return sim.left.value() == ((a >> 3) & 15u) &&
           sim.logical.value() == ((a >> 7) & 15u) &&
           sim.arithmetic.value() == ((a >> 7) & 15u) &&
           sim.signfill.value() == ((a & 0x8000u) ? 3u : 0u);
  };
  if (!check(0) || !check(0x1234) || !check(0x8000) ||
      !check(0xffff)) return 1;
  for (unsigned i = 0; i < 1024; ++i)
    if (!check((i * 251u + 73u) & 65535u)) return 2;
  return 0;
}
CPP
"${CXX:-c++}" -std=c++17 -I "${PYC_TOOLCHAIN_ROOT}/include" -I "${gate_dir}" \
  "${gate_dir}/immediate_shift_slice_harness.cpp" \
  -o "${gate_dir}/immediate_shift_slice_harness"
"${gate_dir}/immediate_shift_slice_harness"
if command -v iverilog >/dev/null 2>&1; then
  "${PYCC}" "${repo_root}/tests/newcircuit/immediate_shift_slice.pyc" \
    --emit=verilog -o "${gate_dir}/immediate_shift_slice.v"
  cat > "${gate_dir}/immediate_shift_slice_tb.sv" <<'SV'
module immediate_shift_slice_tb;
  reg [15:0] a;
  wire [3:0] left, logical, arithmetic;
  wire [1:0] signfill;
  top dut(.a(a), .left(left), .logical(logical),
          .arithmetic(arithmetic), .signfill(signfill));
  initial begin
    a = 16'h1234; #1;
    if (left !== 4'h6 || logical !== 4'h4 ||
        arithmetic !== 4'h4 || signfill !== 2'b00) $fatal;
    a = 16'h8000; #1;
    if (left !== 4'h0 || logical !== 4'h0 ||
        arithmetic !== 4'h0 || signfill !== 2'b11) $fatal;
    a = 16'hx123; #1;
    if (signfill !== 2'bxx) $fatal;
    $finish;
  end
endmodule
SV
  iverilog -g2012 -s immediate_shift_slice_tb \
    -I "${repo_root}/runtime/verilog" \
    -o "${gate_dir}/immediate_shift_slice_tb" \
    "${gate_dir}/immediate_shift_slice.v" \
    "${gate_dir}/immediate_shift_slice_tb.sv"
  vvp "${gate_dir}/immediate_shift_slice_tb" >/dev/null
fi

"${PYCC}" "${repo_root}/tests/newcircuit/immediate_shift_fill.pyc" \
  --emit=cpp --dump-pass-ir="${gate_dir}/immediate_shift_fill_ir" \
  --dump-pass-ir-filter=comb-canonicalize \
  -o "${gate_dir}/immediate_shift_fill.cpp"
if rg -q 'pyc.shli|pyc.lshri|pyc.ashri' \
  "${gate_dir}/immediate_shift_fill_ir"/*after*.mlir; then
  echo 'fixed-shift fill slices retained a full-width shift' >&2
  exit 1
fi
rg -q 'pyc.concat' "${gate_dir}/immediate_shift_fill_ir"/*after*.mlir
rg -q 'pyc.sext' "${gate_dir}/immediate_shift_fill_ir"/*after*.mlir
cat > "${gate_dir}/immediate_shift_fill_harness.cpp" <<'CPP'
#include "immediate_shift_fill.cpp"
int main() {
  pyc::gen::top sim;
  auto check = [&](unsigned a) {
    sim.a = pyc::cpp::Wire<16>(a);
    sim.eval();
    return sim.lowfill.value() == ((a & 1u) << 3) &&
           sim.highfill.value() == ((a >> 14) & 3u) &&
           sim.signfill.value() ==
               (((a >> 14) & 3u) | ((a & 0x8000u) ? 12u : 0u)) &&
           sim.leftzero.value() == 0 && sim.rightzero.value() == 0;
  };
  if (!check(0) || !check(0x1234) || !check(0x8000) ||
      !check(0xffff)) return 1;
  for (unsigned i = 0; i < 1024; ++i)
    if (!check((i * 251u + 73u) & 65535u)) return 2;
  return 0;
}
CPP
"${CXX:-c++}" -std=c++17 -I "${PYC_TOOLCHAIN_ROOT}/include" -I "${gate_dir}" \
  "${gate_dir}/immediate_shift_fill_harness.cpp" \
  -o "${gate_dir}/immediate_shift_fill_harness"
"${gate_dir}/immediate_shift_fill_harness"
if command -v iverilog >/dev/null 2>&1; then
  "${PYCC}" "${repo_root}/tests/newcircuit/immediate_shift_fill.pyc" \
    --emit=verilog -o "${gate_dir}/immediate_shift_fill.v"
  cat > "${gate_dir}/immediate_shift_fill_tb.sv" <<'SV'
module immediate_shift_fill_tb;
  reg [15:0] a;
  wire [3:0] lowfill, highfill, signfill, leftzero, rightzero;
  top dut(.a(a), .lowfill(lowfill), .highfill(highfill),
          .signfill(signfill), .leftzero(leftzero), .rightzero(rightzero));
  initial begin
    a = 16'h8001; #1;
    if (lowfill !== 4'h8 || highfill !== 4'h2 ||
        signfill !== 4'he || leftzero !== 4'h0 ||
        rightzero !== 4'h0) $fatal;
    a = 16'hx123; #1;
    if (lowfill !== 4'h8 || highfill !== 4'b00xx ||
        signfill !== 4'bxxxx || leftzero !== 4'h0 ||
        rightzero !== 4'h0) $fatal;
    $finish;
  end
endmodule
SV
  iverilog -g2012 -s immediate_shift_fill_tb \
    -I "${repo_root}/runtime/verilog" \
    -o "${gate_dir}/immediate_shift_fill_tb" \
    "${gate_dir}/immediate_shift_fill.v" \
    "${gate_dir}/immediate_shift_fill_tb.sv"
  vvp "${gate_dir}/immediate_shift_fill_tb" >/dev/null
fi

# GSIM's concat-equality pattern is a Hardware MLIR rewrite shared by both
# backends. Verify the pass output and sample the resulting circuit.
"${PYCC}" "${repo_root}/tests/newcircuit/eq_concat_constant.pyc" \
  --emit=cpp --dump-pass-ir="${gate_dir}/pattern_ir" \
  --dump-pass-ir-filter=comb-canonicalize -o "${gate_dir}/eq_concat.cpp"
rg -q 'pyc.concat' "${gate_dir}/pattern_ir"/*before*.mlir
if rg -q 'pyc.concat' "${gate_dir}/pattern_ir"/*after*.mlir; then
  echo 'concat equality was not split by the Hardware MLIR pass' >&2
  exit 1
fi
test "$(rg -c 'pyc.eq' "${gate_dir}/pattern_ir"/*after*.mlir)" = 2
cat > "${gate_dir}/eq_concat_harness.cpp" <<'CPP'
#include "eq_concat.cpp"
int main() {
  pyc::gen::top sim;
  sim.a = pyc::cpp::Wire<8>(0x12);
  sim.b = pyc::cpp::Wire<8>(0x34);
  sim.eval();
  if (sim.y.value() != 1) return 1;
  sim.b = pyc::cpp::Wire<8>(0x35);
  sim.eval();
  if (sim.y.value() != 0) return 2;
  sim.a = pyc::cpp::Wire<8>(0x13);
  sim.b = pyc::cpp::Wire<8>(0x34);
  sim.eval();
  return sim.y.value() == 0 ? 0 : 3;
}
CPP
"${CXX:-c++}" -std=c++17 -I "${PYC_TOOLCHAIN_ROOT}/include" -I "${gate_dir}" \
  "${gate_dir}/eq_concat_harness.cpp" -o "${gate_dir}/eq_concat_harness"
"${gate_dir}/eq_concat_harness"
if command -v verilator >/dev/null 2>&1; then
  "${PYCC}" "${repo_root}/tests/newcircuit/eq_concat_constant.pyc" \
    --emit=verilog -o "${gate_dir}/eq_concat.v"
  cat > "${gate_dir}/eq_concat_tb.sv" <<'SV'
module eq_concat_tb;
  reg [7:0] a, b;
  wire y;
  top dut(.a(a), .b(b), .y(y));
  initial begin
    a = 8'h12; b = 8'h34; #1; if (y !== 1'b1) $fatal;
    b = 8'h35; #1; if (y !== 1'b0) $fatal;
    a = 8'h13; b = 8'h34; #1; if (y !== 1'b0) $fatal;
    $finish;
  end
endmodule
SV
  verilator --binary --timing --top-module eq_concat_tb \
    -I"${repo_root}/runtime/verilog" --Mdir "${gate_dir}/eq_concat_vobj" \
    "${gate_dir}/eq_concat.v" "${gate_dir}/eq_concat_tb.sv" >/dev/null
  "${gate_dir}/eq_concat_vobj/Veq_concat_tb" >/dev/null
fi

# A zero-extended shifted field and an independent low field form disjoint
# bits. The Hardware MLIR rewrite must work for both C++ and Verilog.
"${PYCC}" "${repo_root}/tests/newcircuit/eq_shifted_or_constant.pyc" \
  --emit=cpp --dump-pass-ir="${gate_dir}/shifted_or_ir" \
  --dump-pass-ir-filter=comb-canonicalize -o "${gate_dir}/eq_shifted_or.cpp"
test "$(rg -c 'pyc.eq' "${gate_dir}/shifted_or_ir"/*after*.mlir)" = 2
cat > "${gate_dir}/eq_shifted_or_harness.cpp" <<'CPP'
#include "eq_shifted_or.cpp"
int main() {
  pyc::gen::top sim;
  for (unsigned hi = 0; hi < 16; ++hi)
    for (unsigned lo = 0; lo < 16; ++lo) {
      sim.hi = pyc::cpp::Wire<4>(hi);
      sim.lo = pyc::cpp::Wire<4>(lo);
      sim.eval();
      if (sim.y.value() != (hi == 10 && lo == 5)) return 1;
    }
  return 0;
}
CPP
"${CXX:-c++}" -std=c++17 -I "${PYC_TOOLCHAIN_ROOT}/include" -I "${gate_dir}" \
  "${gate_dir}/eq_shifted_or_harness.cpp" -o "${gate_dir}/eq_shifted_or_harness"
"${gate_dir}/eq_shifted_or_harness"
if command -v verilator >/dev/null 2>&1; then
  "${PYCC}" "${repo_root}/tests/newcircuit/eq_shifted_or_constant.pyc" \
    --emit=verilog -o "${gate_dir}/eq_shifted_or.v"
  cat > "${gate_dir}/eq_shifted_or_tb.sv" <<'SV'
module eq_shifted_or_tb;
  reg [3:0] hi, lo;
  wire y;
  top dut(.hi(hi), .lo(lo), .y(y));
  initial begin
    hi = 4'ha; lo = 4'h5; #1; if (y !== 1'b1) $fatal;
    lo = 4'h4; #1; if (y !== 1'b0) $fatal;
    hi = 4'hb; lo = 4'h5; #1; if (y !== 1'b0) $fatal;
    $finish;
  end
endmodule
SV
  verilator --binary --timing --top-module eq_shifted_or_tb \
    -I"${repo_root}/runtime/verilog" --Mdir "${gate_dir}/eq_shifted_or_vobj" \
    "${gate_dir}/eq_shifted_or.v" "${gate_dir}/eq_shifted_or_tb.sv" >/dev/null
  "${gate_dir}/eq_shifted_or_vobj/Veq_shifted_or_tb" >/dev/null
fi
if command -v iverilog >/dev/null 2>&1 && command -v vvp >/dev/null 2>&1; then
  "${PYCC}" "${repo_root}/tests/newcircuit/eq_shifted_or_constant.pyc" \
    --emit=verilog -o "${gate_dir}/eq_shifted_or_x.v"
  cat > "${gate_dir}/eq_shifted_or_x_tb.sv" <<'SV'
module eq_shifted_or_x_tb;
  reg [3:0] hi, lo;
  wire y;
  top dut(.hi(hi), .lo(lo), .y(y));
  initial begin
    hi = 4'bx010; lo = 4'h5; #1; if (y !== 1'bx) $fatal;
    lo = 4'h4; #1; if (y !== 1'b0) $fatal;
    hi = 4'ha; lo = 4'h5; #1; if (y !== 1'b1) $fatal;
    $finish;
  end
endmodule
SV
  iverilog -g2012 -s eq_shifted_or_x_tb -I "${repo_root}/runtime/verilog" \
    -o "${gate_dir}/eq_shifted_or_x" "${gate_dir}/eq_shifted_or_x.v" \
    "${gate_dir}/eq_shifted_or_x_tb.sv"
  vvp "${gate_dir}/eq_shifted_or_x" >/dev/null
fi

"${PYCC}" "${repo_root}/tests/newcircuit/onehot_extract.pyc" \
  --emit=cpp --dump-pass-ir="${gate_dir}/onehot_ir" \
  --dump-pass-ir-filter=comb-canonicalize -o "${gate_dir}/onehot.cpp"
rg -q 'pyc.extract' "${gate_dir}/onehot_ir"/*before*.mlir
if rg -q 'pyc.extract|pyc.shl' "${gate_dir}/onehot_ir"/*after*.mlir; then
  echo 'one-hot extraction was not rewritten by the Hardware MLIR pass' >&2
  exit 1
fi
rg -q 'pyc.eq' "${gate_dir}/onehot_ir"/*after*.mlir
cat > "${gate_dir}/onehot_harness.cpp" <<'CPP'
#include "onehot.cpp"
int main() {
  pyc::gen::top sim;
  sim.shift = pyc::cpp::Wire<8>(3);
  sim.eval();
  if (sim.y.value() != 1) return 1;
  sim.shift = pyc::cpp::Wire<8>(4);
  sim.eval();
  if (sim.y.value() != 0) return 2;
  sim.shift = pyc::cpp::Wire<8>(255);
  sim.eval();
  return sim.y.value() == 0 ? 0 : 3;
}
CPP
"${CXX:-c++}" -std=c++17 -I "${PYC_TOOLCHAIN_ROOT}/include" -I "${gate_dir}" \
  "${gate_dir}/onehot_harness.cpp" -o "${gate_dir}/onehot_harness"
"${gate_dir}/onehot_harness"
if command -v verilator >/dev/null 2>&1; then
  "${PYCC}" "${repo_root}/tests/newcircuit/onehot_extract.pyc" \
    --emit=verilog -o "${gate_dir}/onehot.v"
  cat > "${gate_dir}/onehot_tb.sv" <<'SV'
module onehot_tb;
  reg [7:0] shift;
  wire y;
  top dut(.shift(shift), .y(y));
  initial begin
    shift = 8'd3; #1; if (y !== 1'b1) $fatal;
    shift = 8'd4; #1; if (y !== 1'b0) $fatal;
    shift = 8'd255; #1; if (y !== 1'b0) $fatal;
    $finish;
  end
endmodule
SV
  verilator --binary --timing --top-module onehot_tb \
    -I"${repo_root}/runtime/verilog" --Mdir "${gate_dir}/onehot_vobj" \
    "${gate_dir}/onehot.v" "${gate_dir}/onehot_tb.sv" >/dev/null
  "${gate_dir}/onehot_vobj/Vonehot_tb" >/dev/null
fi

# A feedback loop through two stateful children is legal hardware but cyclic
# for the coarse instance eval graph. Exercise both existing fallback modes.
"${PYCC}" "${repo_root}/tests/newcircuit/scc_state_feedback.pyc" \
  --emit=cpp --logic-depth=256 -o "${gate_dir}/scc.cpp"
rg -q 'eval_fast_scc_path' "${gate_dir}/scc.cpp"
rg -q 'eval_fixpoint_fallback_path' "${gate_dir}/scc.cpp"
cat > "${gate_dir}/scc_harness.cpp" <<'CPP'
#include "scc.cpp"
int main() {
  pyc::gen::top sim;
  sim.clk = pyc::cpp::Wire<1>(1);
  sim.rst = pyc::cpp::Wire<1>(1);
  sim.x = pyc::cpp::Wire<1>(1);
  sim.step();
  if (sim.y.value() != 0) return 1;
  sim.rst = pyc::cpp::Wire<1>(0);
  for (int i = 0; i < 4; ++i) sim.step();
  return sim.y.value() == 0 ? 0 : 2;
}
CPP
"${CXX:-c++}" -std=c++17 -I "${PYC_TOOLCHAIN_ROOT}/include" -I "${gate_dir}" \
  "${gate_dir}/scc_harness.cpp" -o "${gate_dir}/scc_harness"
"${gate_dir}/scc_harness"
PYC_SIM_FAST=1 "${gate_dir}/scc_harness"

# A 257-instance chain crosses the 256-node plan chunk boundary. The last
# instance remains observable, so dead-instance elimination cannot hide it.
python3 - "${repo_root}/tests/newcircuit/scc_state_feedback.pyc" \
  "${gate_dir}/instance_chunks.pyc" <<'PY'
import pathlib
import sys

source = pathlib.Path(sys.argv[1]).read_text()
leaf = source[:source.index("  func.func @top(")]
top_header = source[source.index("  func.func @top("):].splitlines()[0]
top_header = top_header.replace(r'\"module_call_count\":2',
                                r'\"module_call_count\":257')
top_header = top_header.replace(r'\"instance_count\":2',
                                r'\"instance_count\":257')
with pathlib.Path(sys.argv[2]).open("w") as out:
    out.write(leaf)
    out.write(top_header + "\n")
    previous = "%x"
    for index in range(257):
        result = f"%stage{index}"
        out.write(f'    {result} = pyc.instance %clk, %rst, {previous} '
                  f'{{callee = @leaf, name = "u{index}"}} : '
                  '(!pyc.clock, !pyc.reset, i1) -> i1\n')
        previous = result
    out.write(f'    return {previous} : i1\n  }}\n}}\n')
PY
"${PYCC}" "${gate_dir}/instance_chunks.pyc" --emit=cpp --logic-depth=256 \
  -o "${gate_dir}/instance_chunks.cpp"
rg -q 'inline void tick_compute_part_1\(\)' "${gate_dir}/instance_chunks.cpp"
rg -q 'inline void tick_commit_part_1\(\)' "${gate_dir}/instance_chunks.cpp"
test "$(rg -c -- 'u[0-9]+->tick_compute\(\);' "${gate_dir}/instance_chunks.cpp")" = 257
test "$(rg -c -- 'u[0-9]+->tick_commit\(\);' "${gate_dir}/instance_chunks.cpp")" = 257
"${CXX:-c++}" -std=c++17 -I "${PYC_TOOLCHAIN_ROOT}/include" \
  -fsyntax-only "${gate_dir}/instance_chunks.cpp"

# State segmentation is a Hardware MLIR rewrite: C++ and Verilog both contain
# two independent 8-bit registers and preserve reset, enable, and data.
"${PYCC}" "${repo_root}/tests/newcircuit/split_register_slices.pyc" \
  --emit=cpp -o "${gate_dir}/split_register.cpp"
"${PYCC}" "${repo_root}/tests/newcircuit/split_register_slices.pyc" \
  --emit=verilog -o "${gate_dir}/split_register.v"
"${PYCC}" "${repo_root}/tests/newcircuit/split_register_named.pyc" \
  --emit=cpp -o "${gate_dir}/split_register_named.cpp"
"${PYCC}" "${repo_root}/tests/newcircuit/probe_comb_reg.pyc" \
  --emit=cpp -o "${gate_dir}/probe_comb_reg.cpp"
rg -F -q 'reg.addReg<8>(reg_path("y"), &y, &q_inst->pending, &q_inst->qNext)' \
  "${gate_dir}/probe_comb_reg.cpp"
rg -F -q 'reg.addReg<8>(reg_path("q"), &q, &q_inst->pending, &q_inst->qNext)' \
  "${gate_dir}/probe_comb_reg.cpp"
"${CXX:-c++}" -std=c++17 -I "${PYC_TOOLCHAIN_ROOT}/include" \
  -fsyntax-only "${gate_dir}/probe_comb_reg.cpp"
test "$(rg -c 'pyc::cpp::pyc_reg<8> \*' "${gate_dir}/split_register.cpp")" = 2
test "$(rg -c 'pyc_reg #\(\.WIDTH\(8\)\)' "${gate_dir}/split_register.v")" = 2
test "$(rg -c 'pyc::cpp::pyc_reg<16> \*' "${gate_dir}/split_register_named.cpp")" = 1
rg -q '_pyc_reset_group_edge_0' "${gate_dir}/split_register.cpp"
rg -q 'posedge_reset_compute' "${gate_dir}/split_register.cpp"
"${CXX:-c++}" -std=c++17 -I "${PYC_TOOLCHAIN_ROOT}/include" \
  "${repo_root}/tests/newcircuit/reset_batch_runtime.cpp" \
  -o "${gate_dir}/reset_batch_runtime"
"${gate_dir}/reset_batch_runtime"
"${PYCC}" "${repo_root}/tests/newcircuit/reset_batch_vector.pyc" \
  --emit=cpp -o "${gate_dir}/reset_batch_vector.cpp"
test "$(rg -c 'pyc::cpp::pyc_vec_reg<' "${gate_dir}/reset_batch_vector.cpp")" = 4
rg -q '_pyc_reset_group_edge_0' "${gate_dir}/reset_batch_vector.cpp"
"${CXX:-c++}" -std=c++17 -I "${PYC_TOOLCHAIN_ROOT}/include" \
  -fsyntax-only "${gate_dir}/reset_batch_vector.cpp"
cat > "${gate_dir}/split_register_harness.cpp" <<'CPP'
#include "split_register.cpp"
int main() {
  pyc::gen::top sim;
  sim.clk = pyc::cpp::Wire<1>(1);
  sim.rst = pyc::cpp::Wire<1>(1);
  sim.en = pyc::cpp::Wire<1>(1);
  sim.d = pyc::cpp::Wire<16>(0);
  sim.step();
  if (sim.lo.value() != 0x34 || sim.hi.value() != 0x12) return 1;
  sim.rst = pyc::cpp::Wire<1>(0);
  sim.d = pyc::cpp::Wire<16>(0xabcd);
  sim.clk = pyc::cpp::Wire<1>(0);
  sim.step();
  sim.clk = pyc::cpp::Wire<1>(1);
  sim.step();
  if (sim.lo.value() != 0xcd || sim.hi.value() != 0xab) return 2;
  sim.en = pyc::cpp::Wire<1>(0);
  sim.d = pyc::cpp::Wire<16>(0xdead);
  sim.clk = pyc::cpp::Wire<1>(0);
  sim.step();
  sim.clk = pyc::cpp::Wire<1>(1);
  sim.step();
  if (sim.lo.value() != 0xcd || sim.hi.value() != 0xab) return 3;
  sim.rst = pyc::cpp::Wire<1>(1);
  sim.clk = pyc::cpp::Wire<1>(0);
  sim.step();
  sim.clk = pyc::cpp::Wire<1>(1);
  sim.step();
  return sim.lo.value() == 0x34 && sim.hi.value() == 0x12 ? 0 : 4;
}
CPP
"${CXX:-c++}" -std=c++17 -I "${PYC_TOOLCHAIN_ROOT}/include" -I "${gate_dir}" \
  "${gate_dir}/split_register_harness.cpp" -o "${gate_dir}/split_register_harness"
"${gate_dir}/split_register_harness"
if command -v iverilog >/dev/null 2>&1; then
  cat > "${gate_dir}/split_register_tb.sv" <<'SV'
module split_register_tb;
  reg clk = 0, rst = 1, en = 1;
  reg [15:0] d = 0;
  wire [7:0] lo, hi;
  top dut(.clk(clk), .rst(rst), .en(en), .d(d), .lo(lo), .hi(hi));
  initial begin
    #1 clk = 1; #1;
    if (lo !== 8'h34 || hi !== 8'h12) $fatal(1, "reset");
    clk = 0; rst = 0; d = 16'habcd;
    #1 clk = 1; #1;
    if (lo !== 8'hcd || hi !== 8'hab) $fatal(1, "data");
    clk = 0; en = 0; d = 16'hdead;
    #1 clk = 1; #1;
    if (lo !== 8'hcd || hi !== 8'hab) $fatal(1, "enable");
    clk = 0; rst = 1;
    #1 clk = 1; #1;
    if (lo !== 8'h34 || hi !== 8'h12) $fatal(1, "reset priority");
    $finish;
  end
endmodule
SV
  iverilog -g2012 -s split_register_tb -I"${repo_root}/runtime/verilog" \
    -o "${gate_dir}/split_register_tb" "${gate_dir}/split_register.v" \
    "${gate_dir}/split_register_tb.sv"
  "${gate_dir}/split_register_tb"
fi

# A wide register with 65 independent readers has no fixed reader-count cap.
# The hardware pass creates one state lane per bit before both backends.
"${PYCC}" "${gate_dir}/many_register_slices.pyc" \
  --emit=cpp --dump-pass-ir="${gate_dir}/many_register_ir" \
  --dump-pass-ir-filter=comb-canonicalize \
  -o "${gate_dir}/many_register.cpp"
test "$(rg -c 'pyc.reg .* : i1' \
  "${gate_dir}/many_register_ir"/*after*.mlir)" = 65
! rg -q 'pyc.reg .* : i65' "${gate_dir}/many_register_ir"/*after*.mlir
cat > "${gate_dir}/many_register_harness.cpp" <<'CPP'
#include <cstdint>
#include "many_register.cpp"
int main() {
  pyc::gen::top sim;
  sim.clk = pyc::cpp::Wire<1>(1);
  sim.rst = pyc::cpp::Wire<1>(1);
  sim.a = pyc::cpp::Wire<65>({0, 0});
  sim.step();
  for (unsigned bit = 0; bit < 65; ++bit)
    if (sim.y[bit].value() != 0) return 1;
  sim.rst = pyc::cpp::Wire<1>(0);
  for (unsigned step = 0; step < 12; ++step) {
    std::uint64_t low = UINT64_C(0x9e3779b97f4a7c15) * (step + 1);
    unsigned high = step & 1u;
    sim.clk = pyc::cpp::Wire<1>(0);
    sim.step();
    sim.a = pyc::cpp::Wire<65>({low, high});
    sim.clk = pyc::cpp::Wire<1>(1);
    sim.step();
    for (unsigned bit = 0; bit < 65; ++bit) {
      unsigned expected = bit == 64 ? high : ((low >> bit) & 1u);
      if (sim.y[bit].value() != expected) return 2;
    }
  }
  return 0;
}
CPP
"${CXX:-c++}" -std=c++17 -I "${PYC_TOOLCHAIN_ROOT}/include" -I "${gate_dir}" \
  "${gate_dir}/many_register_harness.cpp" -o "${gate_dir}/many_register_harness"
"${gate_dir}/many_register_harness"
if command -v iverilog >/dev/null 2>&1; then
  "${PYCC}" "${gate_dir}/many_register_slices.pyc" \
    --emit=verilog -o "${gate_dir}/many_register.v"
  cat > "${gate_dir}/many_register_tb.sv" <<'SV'
module many_register_tb;
  reg clk = 0, rst = 1;
  reg [64:0] a = 0;
  wire [64:0] y;
  top dut(.clk(clk), .rst(rst), .a(a), .y(y));
  initial begin
    #1 clk = 1; #1;
    if (y !== 65'd0) $fatal(1, "reset");
    clk = 0; rst = 0; a = 65'h1ffffffffffffffff;
    #1 clk = 1; #1;
    if (y !== 65'h1ffffffffffffffff) $fatal(1, "data");
    $finish;
  end
endmodule
SV
  iverilog -g2012 -s many_register_tb \
    -I "${repo_root}/runtime/verilog" \
    -o "${gate_dir}/many_register_tb" \
    "${gate_dir}/many_register.v" "${gate_dir}/many_register_tb.sv"
  vvp "${gate_dir}/many_register_tb" >/dev/null
fi

# Overlapping observations share the middle four state bits rather than
# duplicating them, and both backends reconstruct the same two byte values.
"${PYCC}" "${repo_root}/tests/newcircuit/split_register_overlap.pyc" \
  --emit=cpp -o "${gate_dir}/split_overlap.cpp"
"${PYCC}" "${repo_root}/tests/newcircuit/split_register_overlap.pyc" \
  --emit=verilog -o "${gate_dir}/split_overlap.v"
test "$(rg -c 'pyc::cpp::pyc_reg<4> \*' "${gate_dir}/split_overlap.cpp")" = 3
test "$(rg -c 'pyc_reg #\(\.WIDTH\(4\)\)' "${gate_dir}/split_overlap.v")" = 3
cat > "${gate_dir}/split_overlap_harness.cpp" <<'CPP'
#include "split_overlap.cpp"
int main() {
  pyc::gen::top sim;
  sim.clk = pyc::cpp::Wire<1>(0);
  sim.rst = pyc::cpp::Wire<1>(1);
  sim.en = pyc::cpp::Wire<1>(1);
  sim.d = pyc::cpp::Wire<16>(0);
  auto pulse = [&]() {
    sim.clk = pyc::cpp::Wire<1>(0); sim.step();
    sim.clk = pyc::cpp::Wire<1>(1); sim.step();
  };
  pulse();
  if (sim.lo.value() != 0x34 || sim.mid.value() != 0x23) return 1;
  sim.rst = pyc::cpp::Wire<1>(0);
  sim.d = pyc::cpp::Wire<16>(0xabcd);
  pulse();
  if (sim.lo.value() != 0xcd || sim.mid.value() != 0xbc) return 2;
  sim.en = pyc::cpp::Wire<1>(0);
  sim.d = pyc::cpp::Wire<16>(0xdead);
  pulse();
  if (sim.lo.value() != 0xcd || sim.mid.value() != 0xbc) return 3;
  sim.rst = pyc::cpp::Wire<1>(1);
  pulse();
  return sim.lo.value() == 0x34 && sim.mid.value() == 0x23 ? 0 : 4;
}
CPP
"${CXX:-c++}" -std=c++17 -I "${PYC_TOOLCHAIN_ROOT}/include" -I "${gate_dir}" \
  "${gate_dir}/split_overlap_harness.cpp" -o "${gate_dir}/split_overlap_harness"
"${gate_dir}/split_overlap_harness"
if command -v iverilog >/dev/null 2>&1; then
  cat > "${gate_dir}/split_overlap_tb.sv" <<'SV'
module split_overlap_tb;
  reg clk = 0, rst = 1, en = 1;
  reg [15:0] d = 0;
  wire [7:0] lo, mid;
  top dut(.clk(clk), .rst(rst), .en(en), .d(d), .lo(lo), .mid(mid));
  initial begin
    #1 clk = 1; #1;
    if (lo !== 8'h34 || mid !== 8'h23) $fatal(1, "reset");
    clk = 0; rst = 0; d = 16'habcd;
    #1 clk = 1; #1;
    if (lo !== 8'hcd || mid !== 8'hbc) $fatal(1, "data");
    clk = 0; en = 0; d = 16'hdead;
    #1 clk = 1; #1;
    if (lo !== 8'hcd || mid !== 8'hbc) $fatal(1, "enable");
    clk = 0; rst = 1;
    #1 clk = 1; #1;
    if (lo !== 8'h34 || mid !== 8'h23) $fatal(1, "reset priority");
    $finish;
  end
endmodule
SV
  iverilog -g2012 -s split_overlap_tb -I"${repo_root}/runtime/verilog" \
    -o "${gate_dir}/split_overlap_tb" "${gate_dir}/split_overlap.v" \
    "${gate_dir}/split_overlap_tb.sv"
  "${gate_dir}/split_overlap_tb"
fi

# Rank-one vector state with only fixed-index readers keeps the observed
# lanes; value semantics are checked through both generated backends.
"${PYCC}" "${repo_root}/tests/newcircuit/split_vector_register.pyc" \
  --emit=cpp -o "${gate_dir}/split_vector_register.cpp"
"${PYCC}" "${repo_root}/tests/newcircuit/split_vector_register.pyc" \
  --emit=verilog -o "${gate_dir}/split_vector_register.v"
test "$(rg -c 'pyc::cpp::pyc_reg<8> \*' "${gate_dir}/split_vector_register.cpp")" = 2
test "$(rg -c 'pyc_reg #\(\.WIDTH\(8\)\)' "${gate_dir}/split_vector_register.v")" = 2
cat > "${gate_dir}/split_vector_register_harness.cpp" <<'CPP'
#include "split_vector_register.cpp"
int main() {
  pyc::gen::top sim;
  sim.clk = pyc::cpp::Wire<1>(0);
  sim.rst = pyc::cpp::Wire<1>(1);
  sim.en = pyc::cpp::Wire<1>(1);
  for (int i = 0; i < 4; ++i) sim.d[i] = pyc::cpp::Wire<8>(0);
  auto pulse = [&]() {
    sim.clk = pyc::cpp::Wire<1>(0); sim.step();
    sim.clk = pyc::cpp::Wire<1>(1); sim.step();
  };
  pulse();
  if (sim.lane0.value() != 17 || sim.lane2.value() != 51) return 1;
  sim.rst = pyc::cpp::Wire<1>(0);
  sim.d[0] = pyc::cpp::Wire<8>(0xdd);
  sim.d[2] = pyc::cpp::Wire<8>(0xbb);
  pulse();
  if (sim.lane0.value() != 0xdd || sim.lane2.value() != 0xbb) return 2;
  sim.en = pyc::cpp::Wire<1>(0);
  sim.d[0] = pyc::cpp::Wire<8>(0);
  sim.d[2] = pyc::cpp::Wire<8>(0);
  pulse();
  return sim.lane0.value() == 0xdd && sim.lane2.value() == 0xbb ? 0 : 3;
}
CPP
"${CXX:-c++}" -std=c++17 -I "${PYC_TOOLCHAIN_ROOT}/include" -I "${gate_dir}" \
  "${gate_dir}/split_vector_register_harness.cpp" -o "${gate_dir}/split_vector_register_harness"
"${gate_dir}/split_vector_register_harness"
if command -v iverilog >/dev/null 2>&1; then
  cat > "${gate_dir}/split_vector_register_tb.sv" <<'SV'
module split_vector_register_tb;
  reg clk = 0, rst = 1, en = 1;
  reg [31:0] d = 0;
  wire [7:0] lane0, lane2;
  top dut(.clk(clk), .rst(rst), .en(en), .d(d), .lane0(lane0), .lane2(lane2));
  initial begin
    #1 clk = 1; #1;
    if (lane0 !== 8'h11 || lane2 !== 8'h33) $fatal(1, "reset");
    clk = 0; rst = 0; d = 32'haabbccdd;
    #1 clk = 1; #1;
    if (lane0 !== 8'hdd || lane2 !== 8'hbb) $fatal(1, "data");
    clk = 0; en = 0; d = 0;
    #1 clk = 1; #1;
    if (lane0 !== 8'hdd || lane2 !== 8'hbb) $fatal(1, "enable");
    $finish;
  end
endmodule
SV
  iverilog -g2012 -s split_vector_register_tb -I"${repo_root}/runtime/verilog" \
    -o "${gate_dir}/split_vector_register_tb" "${gate_dir}/split_vector_register.v" \
    "${gate_dir}/split_vector_register_tb.sv"
  "${gate_dir}/split_vector_register_tb"
fi

# A vector expression consumed only through fixed-index reads becomes scalar
# lane operations in the shared hardware pipeline.
"${PYCC}" "${repo_root}/tests/newcircuit/split_vector_elementwise.pyc" \
  --emit=cpp -o "${gate_dir}/split_vector_elementwise.cpp"
"${PYCC}" "${repo_root}/tests/newcircuit/split_vector_elementwise.pyc" \
  --emit=verilog -o "${gate_dir}/split_vector_elementwise.v"
test "$(rg -c 'pyc::cpp::Wire<8> pyc_add_' "${gate_dir}/split_vector_elementwise.cpp")" = 2
cat > "${gate_dir}/split_vector_elementwise_harness.cpp" <<'CPP'
#include "split_vector_elementwise.cpp"
int main() {
  pyc::gen::top sim;
  for (unsigned a = 0; a < 16; ++a)
    for (unsigned b = 0; b < 16; ++b) {
      for (unsigned i = 0; i < 4; ++i) {
        sim.a[i] = pyc::cpp::Wire<8>(a + i * 17);
        sim.b[i] = pyc::cpp::Wire<8>(b + i * 29);
      }
      sim.eval();
      if (sim.lane0.value() != ((a + b) & 255) ||
          sim.lane2.value() != ((a + b + 92) & 255)) return 1;
    }
  return 0;
}
CPP
"${CXX:-c++}" -std=c++17 -I "${PYC_TOOLCHAIN_ROOT}/include" -I "${gate_dir}" \
  "${gate_dir}/split_vector_elementwise_harness.cpp" -o "${gate_dir}/split_vector_elementwise_harness"
"${gate_dir}/split_vector_elementwise_harness"
if command -v iverilog >/dev/null 2>&1; then
  cat > "${gate_dir}/split_vector_elementwise_tb.sv" <<'SV'
module split_vector_elementwise_tb;
  reg [31:0] a, b;
  wire [7:0] lane0, lane2;
  top dut(.a(a), .b(b), .lane0(lane0), .lane2(lane2));
  initial begin
    a = 32'h03020100; b = 32'h07060504; #1;
    if (lane0 !== 8'd4 || lane2 !== 8'd8) $fatal(1, "lanes");
    a = 32'h00ff00ff; b = 32'h00010001; #1;
    if (lane0 !== 8'd0 || lane2 !== 8'd0) $fatal(1, "wrap");
    $finish;
  end
endmodule
SV
  iverilog -g2012 -s split_vector_elementwise_tb -I"${repo_root}/runtime/verilog" \
    -o "${gate_dir}/split_vector_elementwise_tb" "${gate_dir}/split_vector_elementwise.v" \
    "${gate_dir}/split_vector_elementwise_tb.sv"
  "${gate_dir}/split_vector_elementwise_tb"
fi

# Fixed-index reads of a rank-two vector lower through both dimensions.
"${PYCC}" "${repo_root}/tests/newcircuit/split_rank2_register.pyc" \
  --emit=cpp -o "${gate_dir}/rank2_reg.cpp"
"${PYCC}" "${repo_root}/tests/newcircuit/split_rank2_register.pyc" \
  --emit=verilog -o "${gate_dir}/rank2_reg.v"
test "$(rg -c 'pyc::cpp::pyc_reg<8> \*' "${gate_dir}/rank2_reg.cpp")" = 2
test "$(rg -c 'pyc_reg #\(\.WIDTH\(8\)\)' "${gate_dir}/rank2_reg.v")" = 2
cat > "${gate_dir}/rank2_reg_harness.cpp" <<'CPP'
#include "rank2_reg.cpp"
int main() {
  pyc::gen::top sim;
  sim.clk = pyc::cpp::Wire<1>(1);
  sim.rst = pyc::cpp::Wire<1>(1);
  sim.en = pyc::cpp::Wire<1>(1);
  sim.init[0][1] = pyc::cpp::Wire<8>(0x12);
  sim.init[1][2] = pyc::cpp::Wire<8>(0x34);
  sim.step();
  if (sim.lane01.value() != 0x12 || sim.lane12.value() != 0x34) return 1;
  sim.rst = pyc::cpp::Wire<1>(0);
  sim.d[0][1] = pyc::cpp::Wire<8>(0xab);
  sim.d[1][2] = pyc::cpp::Wire<8>(0xcd);
  sim.clk = pyc::cpp::Wire<1>(0); sim.step();
  sim.clk = pyc::cpp::Wire<1>(1); sim.step();
  if (sim.lane01.value() != 0xab || sim.lane12.value() != 0xcd) return 2;
  sim.en = pyc::cpp::Wire<1>(0);
  sim.d[0][1] = pyc::cpp::Wire<8>(0x55);
  sim.clk = pyc::cpp::Wire<1>(0); sim.step();
  sim.clk = pyc::cpp::Wire<1>(1); sim.step();
  return sim.lane01.value() == 0xab && sim.lane12.value() == 0xcd ? 0 : 3;
}
CPP
"${CXX:-c++}" -std=c++17 -I "${PYC_TOOLCHAIN_ROOT}/include" -I "${gate_dir}" \
  "${gate_dir}/rank2_reg_harness.cpp" -o "${gate_dir}/rank2_reg_harness"
"${gate_dir}/rank2_reg_harness"
if command -v iverilog >/dev/null 2>&1; then
  cat > "${gate_dir}/rank2_reg_tb.sv" <<'SV'
module rank2_reg_tb;
  reg clk=0, rst=0, en=0;
  reg [47:0] d=0, init=0;
  wire [7:0] lane01, lane12;
  top dut(.clk(clk), .rst(rst), .en(en), .d(d), .init(init),
          .lane01(lane01), .lane12(lane12));
  initial begin
    init[15:8]=8'h12; init[47:40]=8'h34;
    rst=1; en=1; #1; clk=1; #1;
    if (lane01!==8'h12 || lane12!==8'h34) $fatal(1, "reset");
    rst=0; d[15:8]=8'hab; d[47:40]=8'hcd;
    clk=0; #1; clk=1; #1;
    if (lane01!==8'hab || lane12!==8'hcd) $fatal(1, "data");
    en=0; d[15:8]=8'h55;
    clk=0; #1; clk=1; #1;
    if (lane01!==8'hab || lane12!==8'hcd) $fatal(1, "hold");
    $finish;
  end
endmodule
SV
  iverilog -g2012 -s rank2_reg_tb -I"${repo_root}/runtime/verilog" \
    -o "${gate_dir}/rank2_reg_tb" "${gate_dir}/rank2_reg.v" \
    "${gate_dir}/rank2_reg_tb.sv"
  "${gate_dir}/rank2_reg_tb"
fi
"${PYCC}" "${repo_root}/tests/newcircuit/split_rank2_elementwise.pyc" \
  --emit=cpp -o "${gate_dir}/rank2_elem.cpp"
"${PYCC}" "${repo_root}/tests/newcircuit/split_rank2_elementwise.pyc" \
  --emit=verilog -o "${gate_dir}/rank2_elem.v"
test "$(rg -c 'pyc::cpp::Wire<8> pyc_add_' "${gate_dir}/rank2_elem.cpp")" = 2
cat > "${gate_dir}/rank2_elem_harness.cpp" <<'CPP'
#include "rank2_elem.cpp"
#include <cstdint>
int main() {
  pyc::gen::top sim;
  std::uint32_t random = 0x17ab39e5u;
  for (unsigned i = 0; i < 1000; ++i) {
    random ^= random << 13;
    random ^= random >> 17;
    random ^= random << 5;
    unsigned a = random & 255u, b = (random >> 8) & 255u;
    unsigned c = (random >> 16) & 255u, d = (random >> 24) & 255u;
    sim.a[0][1] = pyc::cpp::Wire<8>(a);
    sim.b[0][1] = pyc::cpp::Wire<8>(b);
    sim.a[1][2] = pyc::cpp::Wire<8>(c);
    sim.b[1][2] = pyc::cpp::Wire<8>(d);
    sim.eval();
    if (sim.lane01.value() != ((a + b) & 255u) ||
        sim.lane12.value() != ((c + d) & 255u)) return 1;
  }
  return 0;
}
CPP
"${CXX:-c++}" -std=c++17 -I "${PYC_TOOLCHAIN_ROOT}/include" -I "${gate_dir}" \
  "${gate_dir}/rank2_elem_harness.cpp" -o "${gate_dir}/rank2_elem_harness"
"${gate_dir}/rank2_elem_harness"
if command -v iverilog >/dev/null 2>&1; then
  cat > "${gate_dir}/rank2_elem_tb.sv" <<'SV'
module rank2_elem_tb;
  reg [47:0] a=0, b=0;
  wire [7:0] lane01, lane12;
  top dut(.a(a), .b(b), .lane01(lane01), .lane12(lane12));
  initial begin
    a[15:8]=8'hff; b[15:8]=8'h01;
    a[47:40]=8'h7f; b[47:40]=8'h02; #1;
    if (lane01!==8'h00 || lane12!==8'h81) $fatal(1, "lanes");
    $finish;
  end
endmodule
SV
  iverilog -g2012 -s rank2_elem_tb -I"${repo_root}/runtime/verilog" \
    -o "${gate_dir}/rank2_elem_tb" "${gate_dir}/rank2_elem.v" \
    "${gate_dir}/rank2_elem_tb.sv"
  "${gate_dir}/rank2_elem_tb"
fi

# Truncating modular arithmetic is equivalent to operating directly at the
# observed width; the hardware pass narrows add/sub/mul for both backends.
"${PYCC}" "${repo_root}/tests/newcircuit/trunc_arithmetic.pyc" \
  --emit=cpp -o "${gate_dir}/trunc_arithmetic.cpp"
"${PYCC}" "${repo_root}/tests/newcircuit/trunc_arithmetic.pyc" \
  --emit=verilog -o "${gate_dir}/trunc_arithmetic.v"
rg -q 'pyc::cpp::Wire<8> pyc_add_' "${gate_dir}/trunc_arithmetic.cpp"
if rg -q 'pyc::cpp::Wire<16> pyc_add_' "${gate_dir}/trunc_arithmetic.cpp"; then
  echo 'truncated arithmetic retained a wide add' >&2
  exit 1
fi
cat > "${gate_dir}/trunc_arithmetic_harness.cpp" <<'CPP'
#include "trunc_arithmetic.cpp"
int main() {
  pyc::gen::top sim;
  for (unsigned a = 0; a < 65536; a += 997)
    for (unsigned b = 0; b < 65536; b += 1021) {
      sim.a = pyc::cpp::Wire<16>(a);
      sim.b = pyc::cpp::Wire<16>(b);
      sim.eval();
      if (sim.y.value() != ((a + b) & 255)) return 1;
    }
  return 0;
}
CPP
"${CXX:-c++}" -std=c++17 -I "${PYC_TOOLCHAIN_ROOT}/include" -I "${gate_dir}" \
  "${gate_dir}/trunc_arithmetic_harness.cpp" -o "${gate_dir}/trunc_arithmetic_harness"
"${gate_dir}/trunc_arithmetic_harness"
if command -v iverilog >/dev/null 2>&1; then
  cat > "${gate_dir}/trunc_arithmetic_tb.sv" <<'SV'
module trunc_arithmetic_tb;
  reg [15:0] a, b;
  wire [7:0] y;
  top dut(.a(a), .b(b), .y(y));
  initial begin
    a = 16'h12ff; b = 16'h3402; #1;
    if (y !== 8'h01) $fatal(1, "carry");
    a = 16'h1234; b = 16'h5678; #1;
    if (y !== 8'hac) $fatal(1, "sum");
    $finish;
  end
endmodule
SV
  iverilog -g2012 -s trunc_arithmetic_tb -I"${repo_root}/runtime/verilog" \
    -o "${gate_dir}/trunc_arithmetic_tb" "${gate_dir}/trunc_arithmetic.v" \
    "${gate_dir}/trunc_arithmetic_tb.sv"
  "${gate_dir}/trunc_arithmetic_tb"
fi

# Deep legal DAGs must not exhaust the compiler stack during the hardware
# legality analysis that precedes SimGraph construction.
python3 - "${repo_root}/tests/newcircuit/group_activation.pyc" \
  "${gate_dir}/large_dag.pyc" <<'PY'
from pathlib import Path
import sys

source = Path(sys.argv[1]).read_text(encoding="utf-8")
header = source.split("    %t = pyc.add", 1)[0]
with Path(sys.argv[2]).open("w", encoding="utf-8") as output:
    output.write(header)
    for i in range(20_000):
        left = "%a" if i == 0 else f"%n{i - 1}"
        output.write(f"    %n{i} = pyc.add {left}, %b : i8, i8 -> i8\n")
    output.write("    return %n19999 : i8\n  }\n}\n")
PY
"${PYCC}" "${gate_dir}/large_dag.pyc" --emit=cpp --logic-depth=25000 \
  -o "${gate_dir}/large_dag.cpp"
rg -q 'inline void eval_sim_group_' "${gate_dir}/large_dag.cpp"

# Packed activity must address groups beyond the first 64-bit word.
python3 - "${repo_root}/tests/newcircuit/group_activation.pyc" \
  "${gate_dir}/packed_multiword.pyc" <<'PY'
from pathlib import Path
import sys

source = Path(sys.argv[1]).read_text(encoding="utf-8")
header = source.split("    %t = pyc.add", 1)[0]
with Path(sys.argv[2]).open("w", encoding="utf-8") as output:
    output.write(header)
    output.write("    %n0 = pyc.add %a, %b : i8, i8 -> i8\n")
    output.write("    %n1 = pyc.xor %n0, %a : i8, i8 -> i8\n")
    for i in range(2, 132):
        output.write(
            f"    %n{i} = pyc.add %n{i - 1}, %n{i - 2} : i8, i8 -> i8\n"
        )
    output.write("    return %n131 : i8\n  }\n}\n")
PY
"${PYCC}" "${gate_dir}/packed_multiword.pyc" --emit=cpp \
  --sim-supernode-max-size=2 --logic-depth=256 \
  -o "${gate_dir}/packed_multiword.cpp"
rg -F -q '_pyc_group_active_flags[1]' "${gate_dir}/packed_multiword.cpp"
cat > "${gate_dir}/packed_multiword_harness.cpp" <<'CPP'
#include "packed_multiword.cpp"
#include <cstdint>
int main() {
  pyc::gen::top sim;
  std::uint32_t random = 0x908ecd31u;
  for (unsigned k = 0; k < 1000; ++k) {
    random ^= random << 13;
    random ^= random >> 17;
    random ^= random << 5;
    unsigned a = random & 255u, b = (random >> 8) & 255u;
    sim.a = pyc::cpp::Wire<8>(a);
    sim.b = pyc::cpp::Wire<8>(b);
    sim.eval();
    unsigned x = (a + b) & 255u, y = x ^ a;
    for (unsigned i = 2; i < 132; ++i) {
      unsigned z = (x + y) & 255u;
      x = y;
      y = z;
    }
    if (sim.y.value() != y) return 1;
  }
  return 0;
}
CPP
"${CXX:-c++}" -std=c++17 -I "${PYC_TOOLCHAIN_ROOT}/include" -I "${gate_dir}" \
  "${gate_dir}/packed_multiword_harness.cpp" -o "${gate_dir}/packed_multiword_harness"
"${gate_dir}/packed_multiword_harness"

echo 'NewCircuit simulation plan gate passed'
