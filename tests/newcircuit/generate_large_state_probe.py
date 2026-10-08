#!/usr/bin/env python3
"""Public synthetic state fanout; no application model input is used."""
import argparse
import json
from pathlib import Path

p = argparse.ArgumentParser()
p.add_argument('out', type=Path)
p.add_argument('--count', type=int, default=1024)
a = p.parse_args()
assert a.count >= 2 and a.count % 2 == 0
a.out.mkdir(parents=True, exist_ok=True)
n = a.count
metrics = {'source_loc': 0, 'ast_node_count': 0, 'hardware_call_count': 0,
           'loop_count': 0, 'module_call_count': 0, 'state_call_count': n,
           'estimated_inline_cost': 0, 'instance_count': 0,
           'state_alloc_count': n, 'collection_count': 0,
           'collection_instance_count': 0, 'module_family_collection_count': 0,
           'repeated_body_clusters': []}
attrs = ('arg_names = ["clk", "rst0", "rst1", "en0", "en1", "en2", "data"], '
         'result_names = ["y"], pyc.kind = "module", pyc.inline = "false", '
         'pyc.params = "{}", pyc.base = "top", pyc.struct.metrics = ' +
         json.dumps(json.dumps(metrics, separators=(',', ':'))) + ', pyc.struct.collections = "[]"')
lines = ['module attributes {pyc.top = @top, pyc.frontend.contract = "pycircuit"} {',
         '  func.func @top(%clk: !pyc.clock, %rst0: !pyc.reset, %rst1: !pyc.reset, '
         '%en0: i1, %en1: i1, %en2: i1, %data: i16) -> i16 attributes {' + attrs + '} {',
         '    %always = pyc.constant 1 : i1']
for i in range(n):
    en = f'%en{i % 4}' if i % 4 != 3 else '%always'
    lines += [f'    %feedback{i} = pyc.wire : i16',
              f'    %salt{i} = pyc.constant {(i * 37 + 11) & 65535} : i16',
              f'    %init{i} = pyc.constant {(i * 13 + 7) & 65535} : i16',
              f'    %input{i} = pyc.add %data, %salt{i} : i16, i16 -> i16',
              f'    %next{i} = pyc.add %feedback{i}, %input{i} : i16, i16 -> i16',
              f'    %q{i} = pyc.reg %clk, %rst{int(i >= n // 2)}, {en}, %next{i}, %init{i} '
              f'{{pyc.name = "state_{i:04}", pyc.debug_keep = true}} : i16',
              f'    pyc.assign %feedback{i}, %q{i} : i16']
lines += ['    return %q0 : i16', '  }', '}']
(a.out / 'large_state_probe.mlir').write_text('\n'.join(lines) + '\n')
(a.out / 'large_state_probe_count.h').write_text(f'#define LARGE_STATE_COUNT {n}\n')
sv = [f'module large_state_probe_tb;', f'  localparam integer N = {n};',
      '  reg clk = 0, rst0, rst1, en0, en1, en2;',
      '  reg [15:0] data;', '  wire [15:0] y;', '  top dut(.*);',
      '  wire [15:0] observed [0:N-1];', '  reg [15:0] expected [0:N-1];',
      "  reg [63:0] digest = 64'd1469598103934665603;",
      '  integer cycle, i;', '  reg reset_value, enable_value;']
for i in range(n):
    sv += [f'  assign observed[{i}] = dut.state_{i:04};']
sv += ['  task check_all;', '    begin', '      for (i = 0; i < N; i = i + 1)',
       '        if (observed[i] !== expected[i])',
       '          $fatal(1, "large state mismatch cycle=%0d lane=%0d", cycle, i);',
       '      if (y !== expected[0]) $fatal(1, "output mismatch");',
       '    end', '  endtask', '  initial begin',
       '    for (i = 0; i < N; i = i + 1) expected[i] = 0;',
       '    for (cycle = 0; cycle < 96; cycle = cycle + 1) begin',
       '      clk = 0; rst0 = cycle % 17 == 0; rst1 = cycle % 23 == 0;',
       '      en0 = cycle % 3 != 0; en1 = cycle % 5 != 0; en2 = cycle % 7 != 0;',
       '      data = cycle * 977 + 31; #1;',
       '      for (i = 0; i < N; i = i + 1) begin',
       '        reset_value = i < N / 2 ? rst0 : rst1;',
       '        case (i % 4)', '          0: enable_value = en0;',
       '          1: enable_value = en1;', '          2: enable_value = en2;',
       '          3: enable_value = 1;', '        endcase',
       '        if (reset_value) expected[i] = i * 13 + 7;',
       '        else if (enable_value) expected[i] = expected[i] + data + i * 37 + 11;',
       '      end', '      clk = 1; #1; check_all();',
       '      for (i = 0; i < N; i = i + 1)',
       "        digest = (digest ^ {48'b0, observed[i]}) * 64'd1099511628211;",
       '      // Stable high clock plus changed data/reset cannot create another edge.',
       "      data = data ^ 16'hffff; rst0 = !rst0; rst1 = !rst1; #1; check_all();",
       '      #1; check_all();', '    end',
       '    $display("Large state/probe oracle PASS: %0d checks; digest=%016h", N * 96, digest);',
       '    $finish;', '  end', 'endmodule']
(a.out / 'large_state_probe_tb.sv').write_text('\n'.join(sv) + '\n')
print(f'Generated public synthetic model: {n} independently observed 16-bit registers')
