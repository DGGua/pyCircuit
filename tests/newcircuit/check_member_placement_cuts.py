#!/usr/bin/env python3
"""Measure edges crossing actual generated methods, without trusting plan counters."""
import argparse
import json
import re
from pathlib import Path
p=argparse.ArgumentParser()
p.add_argument('directory', type=Path)
p.add_argument('--baseline', type=Path)
p.add_argument('--record-baseline', type=Path)
a=p.parse_args()
manifest=json.loads((a.directory/'manifest.json').read_text())
text='\n'.join((a.directory/s['path']).read_text() for s in manifest['sources'])
hpp=(a.directory/'top.hpp').read_text()
widths={name:int(width) for width,name in re.findall(r'pyc::cpp::Wire<(\d+)> (pyc_(?:add|xor)_\d+)\{\};',hpp+'\n'+text)}
methods={name:body for name,body in re.findall(r'^void (?:PYC_NOINLINE )?top::(eval_comb_\d+_part_\d+)\(\)\s*\{(.*?)^[ \t]*\}',text,re.M|re.S)}
assert methods,'missing pure-comb helper chunks'
owners={}
uses={}
for method,body in methods.items():
    definitions=re.findall(r'\b(pyc_(?:add|xor)_\d+) = ([^;]+);',body)
    assert len(definitions)<=4,(method,len(definitions))
    for name,rhs in definitions:
        assert name not in owners,name
        owners[name]=method
        uses[name]=set(re.findall(r'\bpyc_(?:add|xor)_\d+\b',rhs))
assert len(owners)==16,owners
cross={source for target,refs in uses.items() for source in refs if owners[source]!=owners[target]}
report={'expression_count':16,'chunk_cap':4,'cross_method_values':len(cross),
        'cut_weight':sum(1+(widths[name]+63)//64 for name in cross),
        'cut_bit_sum':sum(widths[name] for name in cross),
        'persistent_expression_fields':len(re.findall(r'^  pyc::cpp::Wire<\d+> pyc_(?:add|xor)_\d+\{\};',hpp,re.M)),
        'cross_values':sorted(cross)}
if a.record_baseline:
    assert report['cross_method_values']==12 and report['cut_weight']==27,report
    report['compiler_sha256']='12955eeead33e7fb48fa9f6a939e8d8999885d7fe0e667efea3991f09d88af78'
    a.record_baseline.write_text(json.dumps(report,indent=2)+'\n')
if a.baseline:
    baseline=json.loads(a.baseline.read_text())
    assert report['cross_method_values']<baseline['cross_method_values'],(report,baseline)
    assert report['cut_weight']<baseline['cut_weight'],(report,baseline)
    assert report['persistent_expression_fields']<baseline['persistent_expression_fields'],(report,baseline)
    print('Strict locality improvement PASS: independent generated-method edge/weight/member counts all decrease')
(a.directory/'measured-cuts.json').write_text(json.dumps(report,indent=2)+'\n')
print(json.dumps(report,sort_keys=True))
