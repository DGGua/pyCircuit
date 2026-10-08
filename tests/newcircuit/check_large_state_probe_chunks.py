#!/usr/bin/env python3
"""Check compiler output by actions and order, independent of exact whitespace."""
import argparse
import json
import re
from pathlib import Path

p = argparse.ArgumentParser()
p.add_argument('cpp', type=Path)
p.add_argument('--count', type=int, default=1024)
p.add_argument('--expect-unbounded', action='store_true')
p.add_argument('--split-dir', type=Path)
a = p.parse_args()
text = a.cpp.read_text()
# Erase strings/comments to make braces in diagnostics irrelevant, retaining offsets.
clean = re.sub(r'"(?:\\.|[^"\\])*"|//[^\n]*|/\*.*?\*/', lambda m: ' ' * len(m[0]), text, flags=re.S)
methods = {}
for m in re.finditer(r'^  (?:(?:inline |explicit )?void (?:PYC_NOINLINE )?|)([A-Za-z_][\w]*)\([^\n]*\)(?: const)? \{', clean, re.M):
    depth, end = 1, m.end()
    while depth:
        if clean[end] == '{': depth += 1
        if clean[end] == '}': depth -= 1
        end += 1
    methods[m[1]] = (text[m.start():m.end()], text[m.end():end-1])
lines = {name: body.count('\n') for name, (_, body) in methods.items()}
watched = ('top', 'pyc_register_probes', '_pyc_validate_primitive_bindings', 'tick_compute', 'tick_commit')
print(json.dumps({'public_method_lines': {k: lines[k] for k in watched if k in lines},
                  'max_method_lines': max(lines.values())}, sort_keys=True))
if a.expect_unbounded:
    assert max(lines.get(k, 0) for k in watched) > 1000
    assert '_pyc_init_primitives_part_0' not in methods
    print('Expected pre-fix negative: public state/probe methods exceed 1000 lines and lack bounded helpers')
    raise SystemExit(0)

def family(prefix, action, total, expected_states=None):
    names = sorted((n for n in methods if n.startswith(prefix)), key=lambda n: int(n.rsplit('_', 1)[1]))
    assert names, ('missing bounded family', prefix)
    assert [int(n.rsplit('_', 1)[1]) for n in names] == list(range(len(names))), names
    actions, states = 0, []
    for name in names:
        signature, body = methods[name]
        assert 'void PYC_NOINLINE ' in signature, ('inlinable helper', name)
        count = len(re.findall(action, body))
        assert 0 < count <= 256, (name, count)
        assert lines[name] <= 4096, (name, lines[name])
        actions += count
        if expected_states is not None:
            states += list(map(int, re.findall(r'\bstate_(\d+)_inst', body)))
    assert actions == total, (prefix, actions, total)
    if expected_states is not None:
        # Each action can reference a state more than once (e.g. publication).
        ordered = [v for i, v in enumerate(states) if i == 0 or states[i-1] != v]
        assert ordered == expected_states, (prefix, ordered[:8], ordered[-8:])
    return names

for name in watched:
    assert name in methods and lines[name] < 128, (name, lines.get(name))
init = family('_pyc_init_primitives_part_', r'new pyc::cpp::pyc_reg<', a.count, list(range(a.count)))
validate = family('_pyc_validate_primitive_bindings_part_', r'if \(!state_\d+_inst\)', a.count, list(range(a.count)))
probe = family('pyc_register_probes_part_', r'\breg\.add(?:Wire|Reg|Vec|Alias)(?:<|\()', a.count + 8)
commit = family('tick_local_commit_part_', r'->tick_commit\(\)', a.count, list(range(a.count)))
for group in range(2):
    ids = list(range(group * a.count // 2, (group + 1) * a.count // 2))
    wrapper = methods[f'tick_reset_group_{group}_compute'][1]
    assert wrapper.count(f'_pyc_reset_group_clk_prev_{group} =') == 1
    assert wrapper.count(f'rst{group}.toBool()') == 1
    assert wrapper.count('clk.toBool()') == 1
    for branch, action in [('reset', 'posedge_reset_compute'), ('data', 'posedge_data_compute'),
                           ('noedge', 'noedge_update'), ('negedge', 'negedge_update')]:
        names = family(f'tick_reset_group_{group}_{branch}_part_', rf'->{action}\(\)', len(ids), ids)
        positions = [wrapper.index(name + '();') for name in names]
        assert positions == sorted(positions), (group, branch, positions)
assert '_pyc_init_primitives();' in methods['top'][1]
for wrapper, names in [('_pyc_init_primitives', init), ('_pyc_validate_primitive_bindings', validate),
                       ('pyc_register_probes', probe), ('tick_commit', commit)]:
    body = methods[wrapper][1]
    positions = [body.index(name + '(') for name in names]
    assert positions == sorted(positions), (wrapper, positions)
if a.split_dir:
    manifest = json.loads((a.split_dir / 'manifest.json').read_text())
    core = [a.split_dir / source['path'] for source in manifest['sources'] if source['kind'] == 'core']
    assert len(core) > 1, ('core methods reconcentrated into one translation unit', core)
    all_core = '\n'.join(p.read_text() for p in core)
    for name in init + validate + probe:
        assert len(re.findall(r'\btop::' + name + r'\(', all_core)) == 1, name
    tick = '\n'.join((a.split_dir / source['path']).read_text() for source in manifest['sources'] if source['kind'] == 'tick')
    for name in commit:
        assert len(re.findall(r'\btop::' + name + r'\(', tick)) == 1, name
print('Large state/probe bounded helper structure PASS (256 actions, noinline, ordered complete coverage)')
