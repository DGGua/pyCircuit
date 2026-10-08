#!/usr/bin/env python3
"""Check publication placement through the exact generated tick helper calls."""
from pathlib import Path
import re
import sys

source = Path(sys.argv[1]).read_text()
methods = {}
for match in re.finditer(
    r'^  (?:inline )?void (?:PYC_NOINLINE )?(tick_\w+)\(\) \{', source, re.M
):
    start = match.end()
    depth, end = 1, start
    while depth:
        assert end < len(source), 'unterminated tick method'
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    name = match[1]
    assert name not in methods, f'ambiguous tick method {name}'
    methods[name] = source[start:end - 1]

helper_call = re.compile(r'^[ \t]*(tick_\w+)\(\);[ \t]*$', re.M)


def expand(name, stack=()):
    assert name in methods, f'missing tick helper {name}'
    assert name not in stack, f'recursive tick helper {name}'
    return helper_call.sub(
        lambda m: expand(m[1], (*stack, name)), methods[name]
    )


for stage in ('compute', 'commit'):
    # Each declared chunk must be invoked exactly once and in planned order.
    for prefix in (f'tick_{stage}_part_', f'tick_local_{stage}_part_'):
        expected = sorted(
            (name for name in methods if name.startswith(prefix)),
            key=lambda name: int(name[len(prefix):]),
        )
        actual = [
            call for call in helper_call.findall(methods[f'tick_{stage}'])
            if call.startswith(prefix)
        ]
        assert actual == expected, f'{stage} chunk order/coverage changed'

# The former checks looked only inside the wrappers. Follow their exact calls
# now that bounded methods own the state transitions and their publications.
compute, commit = expand('tick_compute'), expand('tick_commit')
assert '_pyc_old_group_value_' not in compute, 'compute publishes state activity'
assert '_pyc_group_active_flags' not in compute, 'compute modifies activity flags'
snapshots = list(re.finditer(r'auto (_pyc_old_group_value_\d+) = [^;]+;', commit))
assert snapshots, 'state commit lacks activity snapshots'
transition = re.compile(r'(?:->|\.)tick_commit\(\);')
publish = re.compile(r'_pyc_group_active_flags\[\d+\] \|=')
for snapshot in snapshots:
    state_change = transition.search(commit, snapshot.end())
    comparison = re.search(re.escape(snapshot[1]) + r'\b', commit[snapshot.end():])
    assert state_change and comparison, 'snapshot lacks transition/comparison'
    compared_at = snapshot.end() + comparison.start()
    assert state_change.end() <= compared_at, 'activity compared before commit'
    publication = publish.search(commit, compared_at)
    next_transition = transition.search(commit, state_change.end())
    assert publication, 'changed state lacks activity publication'
    assert not next_transition or publication.start() < next_transition.start(), (
        'activity publication moved past the following state transition'
    )
