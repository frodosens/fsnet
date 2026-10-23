#!/usr/bin/env python3
"""Alternate two saved Release builds and retain all attribute benchmark samples."""
import argparse
import datetime
import hashlib
import json
import os
from pathlib import Path
import platform
import statistics
import subprocess

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('before', type=Path)
parser.add_argument('after', type=Path)
parser.add_argument('--runs', type=int, default=7)
parser.add_argument('--iterations', type=int, default=10000)
parser.add_argument('--output', type=Path, required=True)
parser.add_argument('--before-revision', default='unspecified')
parser.add_argument('--after-revision', default='unspecified')
args = parser.parse_args()
if args.runs < 3 or not 100 <= args.iterations <= 1000000:
    parser.error('use at least 3 runs and 100..1000000 iterations')
binaries = {'before': args.before.resolve(), 'after': args.after.resolve()}
samples = {'before': [], 'after': []}

def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()

def invoke(label):
    binary = binaries[label]
    env = dict(os.environ)
    env['LD_LIBRARY_PATH'] = str(binary.parent) + (':' + env['LD_LIBRARY_PATH'] if env.get('LD_LIBRARY_PATH') else '')
    command = [str(binary), 'all', str(args.iterations)]
    completed = subprocess.run(command, env=env, capture_output=True, text=True, timeout=120, check=True)
    rows = [json.loads(line) for line in completed.stdout.splitlines()]
    names = [row['scenario'] for row in rows]
    if len(set(names)) != len(names) or not any('ns_per_op' in row for row in rows):
        raise RuntimeError('invalid benchmark output')
    return {row['scenario']: row for row in rows}

# One untimed process per binary warms source/dependency pages and all code paths.
for label in binaries:
    invoke(label)
for run in range(args.runs):
    order = ('before', 'after') if run % 2 == 0 else ('after', 'before')
    for label in order:
        samples[label].append(invoke(label))
    print(f'completed pair {run + 1}/{args.runs}', flush=True)

if samples['before'][0].keys() != samples['after'][0].keys():
    raise RuntimeError('scenario sets differ')
summary = {}
for name, row in samples['before'][0].items():
    if 'ns_per_op' not in row:
        if name == 'payload_sizes' and row != samples['after'][0][name]:
            raise RuntimeError('serialized sizes changed between builds')
        continue
    summary[name] = {}
    for label in samples:
        values = [sample[name]['ns_per_op'] for sample in samples[label]]
        summary[name][label] = {
            'median_ns': statistics.median(values),
            'min_ns': min(values), 'max_ns': max(values),
        }
        for key in ('p50_us', 'p99_us'):
            if key in row:
                summary[name][label][key] = statistics.median(sample[name][key] for sample in samples[label])
    before = summary[name]['before']['median_ns']
    after = summary[name]['after']['median_ns']
    summary[name]['speedup'] = before / after
    summary[name]['time_reduction_percent'] = (1 - after / before) * 100
    print(f'{name}: {before / 1000:.3f} -> {after / 1000:.3f} us/op, {before / after:.2f}x')

output = {
    'timestamp_utc': datetime.datetime.now(datetime.timezone.utc).isoformat(),
    'platform': platform.platform(),
    'cpu_model': next((line.split(':', 1)[1].strip() for line in Path('/proc/cpuinfo').read_text().splitlines()
                       if line.startswith('model name')), 'unknown') if Path('/proc/cpuinfo').exists() else 'unknown',
    'cpu_affinity': sorted(os.sched_getaffinity(0)) if hasattr(os, 'sched_getaffinity') else None,
    'runs_per_binary': args.runs, 'iterations_argument': args.iterations,
    'before_revision': args.before_revision, 'after_revision': args.after_revision,
    'binaries': {label: {'path': str(binary), 'sha256': digest(binary),
                        'libfsnet_sha256': digest(binary.parent / 'libfsnet.so')}
                 for label, binary in binaries.items()},
    'summary': summary, 'samples': samples,
}
args.output.parent.mkdir(parents=True, exist_ok=True)
args.output.write_text(json.dumps(output, indent=2) + '\n')
print(f'raw results: {args.output}')
