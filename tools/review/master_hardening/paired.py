#!/usr/bin/env python3
"""Rebuild a baseline core using a configured Release toolchain, then alternate A/B.
No checkout is changed. Dependencies and benchmark executables are identical.
Run --build-only first, and measure only after other builds/tests have finished.
"""
import argparse
import concurrent.futures
import hashlib
import json
import os
from pathlib import Path
import platform
import shlex
import subprocess
import tarfile
import time

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('build', type=Path)
p.add_argument('work', type=Path)
p.add_argument('--revision', default='8e4af0c9b73445d999a9e3593c1429c63bbec882')
p.add_argument('--build-only', action='store_true')
a = p.parse_args()
root = Path(__file__).resolve().parents[3]
build, work = a.build.resolve(), a.work.resolve()
work.mkdir(parents=True, exist_ok=True)
baseline = work / 'baseline'
baseline.mkdir(exist_ok=True)
source = baseline / 'source'
revision = subprocess.check_output(['git', 'rev-parse', a.revision], cwd=root, text=True).strip()
if a.build_only:
    reuse = (baseline / 'revision').exists() and (baseline / 'revision').read_text().strip() == revision
    archive = baseline / 'source.tar'
    subprocess.run(['git', 'archive', '--format=tar', '-o', str(archive), revision], cwd=root, check=True)
    source.mkdir(exist_ok=True)
    with tarfile.open(archive) as tar:
        tar.extractall(source, filter='data')
    commands = json.loads((build / 'compile_commands.json').read_text())
    jobs, replacements = [], {}
    for entry in commands:
        if '/CMakeFiles/fsnet.dir/' not in entry['output']:
            continue
        original = Path(entry['file'])
        target = baseline / (original.name + '.o')
        command = shlex.split(entry['command'])
        for i, arg in enumerate(command):
            if arg == str(original):
                command[i] = str(source / original.relative_to(root))
            elif arg in ('-I' + str(root / 'include'), '-I' + str(root / 'src')):
                command[i] = '-I' + str(source / Path(arg[2:]).relative_to(root))
        command[command.index('-o') + 1] = str(target)
        replacements[str(Path(entry['output']).relative_to(build))] = str(target)
        if not reuse or not target.exists():
            jobs.append(command)
    def compile_one(command):
        subprocess.run(command, cwd=build, check=True)
    with concurrent.futures.ThreadPoolExecutor(max_workers=2) as pool:
        list(pool.map(compile_one, jobs))
    cache = (build / 'CMakeCache.txt').read_text().splitlines()
    ninja = next(line.split('=', 1)[1] for line in cache if line.startswith('CMAKE_MAKE_PROGRAM:'))
    link = subprocess.check_output([ninja, '-t', 'commands', 'libfsnet.so'], cwd=build, text=True).splitlines()[-1]
    command = shlex.split(link)[2:-2]  # Ninja's ': && compiler ... && :'
    command = [replacements.get(arg, arg) for arg in command if not arg.startswith('-Wl,--dependency-file=')]
    command[command.index('-o') + 1] = str(baseline / 'libfsnet.so')
    subprocess.run(command, cwd=build, check=True)
    # Use the baseline benchmark fixture with both libraries. The final fixture
    # includes new negative regression checks which intentionally fail on A.
    entry = next(item for item in commands if item['file'] == str(root / 'tests/global_flow.cpp'))
    flow_object = work / 'global_flow.cpp.o'
    command = shlex.split(entry['command'])
    command = [str(source / 'tests/global_flow.cpp') if arg == entry['file'] else
               '-I' + str(source / 'include') if arg == '-I' + str(root / 'include') else arg for arg in command]
    command[command.index('-o') + 1] = str(flow_object)
    subprocess.run(command, cwd=build, check=True)
    link = subprocess.check_output([ninja, '-t', 'commands', 'fsnet-global-flow-tests'], cwd=build, text=True).splitlines()[-1]
    command = shlex.split(link)[2:-2]
    original_object = str(Path(entry['output']).relative_to(build))
    command = [str(flow_object) if arg == original_object else arg for arg in command if not arg.startswith('-Wl,--dependency-file=')]
    command[command.index('-o') + 1] = str(work / 'flow-benchmark')
    subprocess.run(command, cwd=build, check=True)
    (baseline / 'revision').write_text(revision + '\n')
    print('baseline core rebuilt:', revision)
    raise SystemExit()

assert (baseline / 'revision').read_text().strip() == revision
commands = [('native', [str(build / 'fsnet-benchmark'), '100000']),
            ('gateway', [str(build / 'fsnet-request-benchmark'), 'gateway', '20000', '128', '32']),
            ('flow', [str(work / 'flow-benchmark'), str(root), '--benchmark'])]
variants = {'baseline': baseline, 'final': build}
report = {'baseline_revision': revision, 'platform': platform.platform(),
          'cpu_max': Path('/sys/fs/cgroup/cpu.max').read_text().strip(),
          'method': 'same executables and pinned dependencies; baseline core fully recompiled from git archive; one warmup per command/variant, 5 alternating AB/BA pairs; no concurrent builds/tests',
          'scope': 'Release; native and Gateway echo; flow = Lua/Memory CAS, 2000 requests/concurrency 8, loopback without TLS',
          'sha256': {name: hashlib.sha256((directory / 'libfsnet.so').read_bytes()).hexdigest() for name, directory in variants.items()},
          'executable_sha256': {name: hashlib.sha256(Path(cmd[0]).read_bytes()).hexdigest() for name, cmd in commands}, 'runs': []}

def run(variant, command):
    env = dict(os.environ, LD_LIBRARY_PATH=str(variants[variant]))
    return subprocess.run(command, cwd=root, env=env, capture_output=True, text=True, timeout=60, check=True)

for name, command in commands:
    for variant in variants:
        run(variant, command)
for repeat in range(5):
    for name, command in commands:
        for variant in (['baseline', 'final'] if repeat % 2 == 0 else ['final', 'baseline']):
            start = time.monotonic()
            result = run(variant, command)
            report['runs'].append({'repeat': repeat + 1, 'scenario': name, 'variant': variant,
                                   'command': command, 'elapsed_seconds': time.monotonic() - start,
                                   'stdout': result.stdout, 'stderr': result.stderr, 'returncode': result.returncode})
            (work / 'paired.json').write_text(json.dumps(report, indent=2) + '\n')
    print(f'completed pair {repeat + 1}/5', flush=True)
