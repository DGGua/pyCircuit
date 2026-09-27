#!/usr/bin/env python3
"""Generate scalar demand boundary cases and their independent model metadata."""
import json
from pathlib import Path
import sys

out = Path(sys.argv[1])
out.mkdir(parents=True, exist_ok=True)
metrics = dict.fromkeys([
    "source_loc", "ast_node_count", "hardware_call_count", "loop_count",
    "module_call_count", "state_call_count", "estimated_inline_cost",
    "instance_count", "state_alloc_count", "collection_count",
    "collection_instance_count", "module_family_collection_count"], 0)
metrics["repeated_body_clusters"] = []
cases = []


def add(name, body, *, model=0, state=False, full=False, keep=False,
        add_width=8, comb=False):
    cases.append(dict(name=name, model=model, state=int(state), full=int(full),
                      keep=keep, add_width=add_width, comb=comb))
    case_metrics = dict(metrics)
    case_metrics["state_call_count"] = int(state)
    case_metrics["state_alloc_count"] = int(state)
    attrs = ('arg_names = ["clk", "rst", "en", "sel", "a", "b"], '
             'result_names = ["y", "wide"], pyc.kind = "module", '
             'pyc.inline = "false", pyc.params = "{}", pyc.base = "top", '
             'pyc.struct.collections = "[]", '
             f'pyc.struct.metrics = {json.dumps(json.dumps(case_metrics))}')
    (out / f'{name}.mlir').write_text(
        'module attributes {pyc.top = @top, pyc.frontend.contract = "pycircuit"} {\n'
        '  func.func @top(%clk: !pyc.clock, %rst: !pyc.reset, %en: i1, '
        f'%sel: i1, %a: i64, %b: i64) -> (i8, i64) attributes {{{attrs}}} {{\n'
        '    %zero = pyc.constant 0 : i64\n' + body +
        f'    return %y, {"%n" if full else "%zero"} : i8, i64\n'
        '  }\n}\n')


extract = '    %y = pyc.extract %n {lsb = 0 : i64, msb = 7 : i64} : i64 -> i8\n'
trunc = '    %y = pyc.trunc %n : i64 -> i8\n'
sum_op = '    %sum = pyc.add %a, %b : i64, i64 -> i64\n'
for reader in ('extract', 'trunc', 'fullwidth', 'kept'):
    keep = reader == 'kept'
    full = reader == 'fullwidth'
    attrs = ' {pyc.debug_keep}' if keep else ''
    body = sum_op + f'    %n = pyc.reg %clk, %rst, %en, %sum, %b{attrs} : i64\n'
    add(f'reg_{reader}', body + (extract if reader == 'extract' else trunc),
        state=True, full=full, keep=keep)

for kind in ('mux', 'select'):
    for reader in (('trunc',) if kind == 'mux' else ('extract', 'trunc', 'fullwidth', 'kept')):
        keep = reader == 'kept'
        full = reader == 'fullwidth'
        attrs = ' {pyc.debug_keep}' if keep else ''
        operation = (f'pyc.mux %sel, %sum, %a{attrs} : i1, i64, i64 -> i64'
                     if kind == 'mux' else f'arith.select %sel, %sum, %a{attrs} : i64')
        add(f'{kind}_{reader}', sum_op + f'    %n = {operation}\n' +
            (extract if reader == 'extract' else trunc), model=1, full=full, keep=keep)

for reader in ('trunc', 'extract'):
    body = sum_op + '    %n = pyc.zext %sum : i64 -> i128\n'
    body += ('    %y = pyc.trunc %n : i128 -> i8\n' if reader == 'trunc' else
             '    %y = pyc.extract %n {lsb = 0 : i64, msb = 7 : i64} : i128 -> i8\n')
    add(f'cast_{reader}', body)
add('shli_trunc', sum_op + '    %n = pyc.shli %sum {amount = 3 : i64} : i64\n' + trunc,
    model=2, add_width=5)
add('concat_trunc', sum_op +
    '    %n = pyc.concat (%a, %sum) : (i64, i64) -> i128\n'
    '    %y = pyc.trunc %n : i128 -> i8\n')

comb_add = '''    %n = pyc.comb(%a, %b) : (i64, i64) -> i64 {
    ^bb0(%x: i64, %z: i64):
      %inner = pyc.add %x, %z : i64, i64 -> i64
      pyc.yield %inner : i64
    }
'''
add('comb_output', comb_add + extract, comb=True)
add('comb_fullwidth', comb_add + trunc, full=True, comb=True)
add('comb_internal_kept', comb_add.replace('pyc.add %x, %z :',
    'pyc.add %x, %z {pyc.debug_keep} :') + trunc, keep=True, comb=True)
add('comb_input', sum_op + '''    %y = pyc.comb(%sum) : (i64) -> i8 {
    ^bb0(%x: i64):
      %low = pyc.trunc %x : i64 -> i8
      pyc.yield %low : i8
    }
''', comb=True)
add('comb_passthrough', sum_op + '''    %n = pyc.comb(%sum) : (i64) -> i64 {
    ^bb0(%x: i64):
      pyc.yield %x : i64
    }
''' + trunc, comb=True)
add('comb_nested', sum_op + '''    %n = pyc.comb(%sum, %a, %b, %sel) : (i64, i64, i64, i1) -> i64 {
    ^bb0(%x: i64, %u: i64, %v: i64, %c: i1):
      %wrapped = pyc.comb(%x, %u, %v, %c) : (i64, i64, i64, i1) -> i64 {
      ^bb0(%xx: i64, %uu: i64, %vv: i64, %cc: i1):
        %plus = pyc.add %xx, %uu : i64, i64 -> i64
        %times = pyc.mul %xx, %vv : i64, i64 -> i64
        %chosen = arith.select %cc, %plus, %times : i64
        pyc.yield %chosen : i64
      }
      pyc.yield %wrapped : i64
    }
''' + trunc, model=3, comb=True)
(out / 'cases.json').write_text(json.dumps(cases, indent=2) + '\n')
for case in cases:
    print(case['name'], case['model'], case['state'], case['full'], sep='\t')
