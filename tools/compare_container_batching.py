#!/usr/bin/env python3
"""Compare sequential setters and explicit batching using one Release binary."""
import argparse
import datetime
import hashlib
import json
import os
from pathlib import Path
import platform
import statistics
import subprocess

parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('binary',type=Path)
parser.add_argument('--runs',type=int,default=7)
parser.add_argument('--rounds',type=int,default=100)
parser.add_argument('--output',type=Path,required=True)
args=parser.parse_args()
if args.runs<3 or not 10<=args.rounds<=10000:
    parser.error('use at least 3 runs and 10..10000 rounds')
binary=args.binary.resolve()
env=dict(os.environ)
env['LD_LIBRARY_PATH']=str(binary.parent)+(':'+env['LD_LIBRARY_PATH'] if env.get('LD_LIBRARY_PATH') else '')
def invoke(mode,rounds):
    result=subprocess.run([str(binary),mode,str(rounds)],env=env,capture_output=True,text=True,check=True,timeout=120)
    return [json.loads(line) for line in result.stdout.splitlines()]

wire=invoke('wire',10)
for mode in ('sequential','batch'):
    invoke(mode,10)
samples={'sequential':[],'batch':[]}
for run in range(args.runs):
    for mode in (('sequential','batch') if run%2==0 else ('batch','sequential')):
        row,=invoke(mode,args.rounds)
        expected=args.rounds*(16 if mode=='sequential' else 1)
        if row['writes']!=expected or row['commits']!=expected:
            raise RuntimeError('unexpected number of writes/commits')
        samples[mode].append(row)
    print(f'completed pair {run+1}/{args.runs}',flush=True)
summary={}
for mode,rows in samples.items():
    times=[r['us_per_round'] for r in rows]
    summary[mode]={'median_us_per_round':statistics.median(times),'min_us':min(times),'max_us':max(times),
                   'writes_per_round':rows[0]['writes']/args.rounds,
                   'commits_per_round':rows[0]['commits']/args.rounds,
                   'proposal_fsa_bytes_per_round':rows[0]['proposal_fsa_bytes']/args.rounds}
summary['speedup']=summary['sequential']['median_us_per_round']/summary['batch']['median_us_per_round']
record={'timestamp_utc':datetime.datetime.now(datetime.timezone.utc).isoformat(),
        'platform':platform.platform(),
        'cpu_model':next((line.split(':',1)[1].strip() for line in Path('/proc/cpuinfo').read_text().splitlines() if line.startswith('model name')),'unknown'),
        'affinity':sorted(os.sched_getaffinity(0)), 'runs':args.runs,'rounds':args.rounds,
        'binary':str(binary),'binary_sha256':hashlib.sha256(binary.read_bytes()).hexdigest(),
        'libfsnet_sha256':hashlib.sha256((binary.parent/'libfsnet.so').read_bytes()).hexdigest(),
        'comparison':'same Release build, sequential16 versus batch16; post-patch protocol for both',
        'summary':summary,'wire':wire,'samples':samples}
args.output.parent.mkdir(parents=True,exist_ok=True)
args.output.write_text(json.dumps(record,indent=2)+'\n')
print(json.dumps(summary,indent=2))
print(f'raw results: {args.output}')
