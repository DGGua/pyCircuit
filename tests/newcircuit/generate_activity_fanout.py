"""Exercise packed publication decisions with one register and many readers."""
import json
from pathlib import Path
import sys

path = Path(sys.argv[1])
count = int(sys.argv[2])
metrics = {key: 0 for key in (
    "source_loc ast_node_count hardware_call_count loop_count module_call_count "
    "state_call_count estimated_inline_cost instance_count state_alloc_count "
    "collection_count collection_instance_count module_family_collection_count"
).split()}
metrics["repeated_body_clusters"] = []
attrs = ('arg_names = ["clk", "rst", "en", "d"], result_names = ["out"], '
         'pyc.kind = "module", pyc.inline = "false", pyc.params = "{}", '
         'pyc.base = "top", pyc.struct.collections = "[]", pyc.struct.metrics = '
         + json.dumps(json.dumps(metrics)))
body = ['    %init = pyc.constant 19 : i16',
        '    %q = pyc.reg %clk, %rst, %en, %d, %init : i16']
for i in range(count):
    prior = '%q' if i == 0 else f'%v{i - 1}'
    body.append(f'    %v{i} = pyc.add %q, {prior} : i16, i16 -> i16')
values = ', '.join(f'%v{i}' for i in range(count))
types = ', '.join('i16' for _ in range(count))
body += [f'    %out = pyc.v_create({values}) : ({types}) -> vector<{count}xi16>',
         f'    return %out : vector<{count}xi16>']
path.write_text('module attributes {pyc.top = @top, pyc.frontend.contract = "pycircuit"} {\n'
                f'  func.func @top(%clk: !pyc.clock, %rst: !pyc.reset, %en: i1, %d: i16) '
                f'-> vector<{count}xi16> attributes {{{attrs}}} {{\n'
                + '\n'.join(body) + '\n  }\n}\n')
