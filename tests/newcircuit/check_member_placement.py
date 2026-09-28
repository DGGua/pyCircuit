#!/usr/bin/env python3
"""Validate final plan placement through generated declarations and manifest facts."""
import json
import re
import sys
from pathlib import Path
root = Path(sys.argv[1])
summaries = {}
for mode in ('flat', 'crosschunk', 'nested'):
    directory = root / mode / 'split'
    manifest = json.loads((directory / 'manifest.json').read_text())
    placement = manifest.get('profile_summary', {}).get('cpp_placement')
    assert isinstance(placement, dict), (mode, 'missing plan placement summary')
    for field in ('struct_members', 'local_in_method', 'probe_pinned_struct',
                  'cross_part_promoted', 'scheduled_cross_method', 'scheduled_cut_weight'):
        assert field in placement, (mode, field)
        assert isinstance(placement[field], (int, float)) and placement[field] >= 0
    assert placement['local_in_method'] > 0, (mode, 'no actual method locals')
    assert placement['probe_pinned_struct'] >= 3, (mode, 'three named probes must remain materialized')
    hpp = (directory / 'top.hpp').read_text()
    sources = '\n'.join((directory / source['path']).read_text() for source in manifest['sources'])
    members = set(re.findall(r'^  pyc::cpp::Wire<\d+> (pyc_\w+)\{\};', hpp, re.M))
    locals_ = set(re.findall(r'^\s+pyc::cpp::Wire<\d+> (pyc_\w+)\{\};', sources, re.M))
    assert locals_ and not (locals_ & members), (mode, locals_ & members)
    for name, width in [('debug_named', 8), ('debug_nested', 8), ('debug_wide', 128)]:
        assert f'pyc::cpp::Wire<{width}> {name}{{}};' in hpp, (mode, name)
        assert f'reg_path("{name}")' in sources, (mode, name, 'missing registration')
    # Public values and cache inputs survive skipped eval calls.
    for name in ('out', 'nested_out', 'wide_out', 'a', 'b', 'c', 'sel', 'x', 'z'):
        assert re.search(r'\b' + name + r'\{\};', hpp), (mode, name)
    assert '_pyc_group_0_in_' in hpp
    if mode == 'crosschunk':
        assert placement['cross_part_promoted'] > 0, placement
        assert placement['scheduled_cross_method'] > 0 and placement['scheduled_cut_weight'] > 0, placement
        assert 'eval_sim_group_0_part_1' in sources
        # At least one 128-bit intermediate must bridge separate chunk methods.
        # The checker uses actual declarations, allowing the planner to reorder
        # same-owner expressions when it reduces weighted cuts.
        assert re.search(r'^  pyc::cpp::Wire<128> pyc_(?:concat|add|xor|constant)_\w+\{\};', hpp, re.M)
    if mode == 'nested':
        assert 'eval_comb_' in sources, 'nested comb path was not exercised'
    summaries[mode] = dict(placement, observed_local_declarations=len(locals_))
(root / 'placement-summary.json').write_text(json.dumps(summaries, indent=2) + '\n')
print('Member placement structure PASS: method locals, pinned probes/cache interfaces, cross-chunk weighted cuts')
