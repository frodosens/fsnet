#!/usr/bin/env python3
"""Run Release engine baselines serially; save raw samples without production claims."""
import argparse
import hashlib
import json
from pathlib import Path
import platform
import os
import subprocess
import time
import tempfile

parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('build',type=Path)
parser.add_argument('--runs',type=int,default=3)
parser.add_argument('--receiver',type=Path,help='optional compiled native receive diagnostic')
parser.add_argument('--output',type=Path,required=True)
args=parser.parse_args()
if not 3<=args.runs<=20:parser.error('runs must be 3..20')
build=args.build.resolve()
commands=[('fsnet-benchmark',['100000']),('fsnet-attribute-benchmark',['all','20000']),('fsnet-container-benchmark',['all','100']),('fsnet-envelope-benchmark',[]),('fsnet-replication-benchmark',[])]
for size,count in ((128,20000),(4096,10000),(65536,1000)):
 for mode in ('direct','gateway'):commands.append(('fsnet-request-benchmark',[mode,str(count),str(size),'32']))
for mode in ('direct','gateway'):commands.append(('fsnet-request-benchmark',[mode,'5000','128','1']))
executables={name:build/name for name,_ in commands}
if args.receiver:
 receiver=args.receiver.resolve()
 if not receiver.is_file():parser.error('--receiver executable does not exist')
 if receiver.name in executables:parser.error('--receiver name conflicts with an existing benchmark')
 commands.append((receiver.name,[]));executables[receiver.name]=receiver
environment=dict(os.environ)
environment['LD_LIBRARY_PATH']=str(build)+((':'+environment['LD_LIBRARY_PATH']) if environment.get('LD_LIBRARY_PATH') else '')
report={'platform':platform.platform(),'source_revision':subprocess.check_output(['git','rev-parse','HEAD'],text=True).strip(),
        'cpu':next((s.split(':',1)[1].strip() for s in Path('/proc/cpuinfo').read_text().splitlines() if s.startswith('model name')),''),
        'cpu_quota':Path('/sys/fs/cgroup/cpu.max').read_text().strip() if Path('/sys/fs/cgroup/cpu.max').exists() else 'unavailable',
        'compiler':subprocess.check_output([os.environ.get('CXX','c++'),'--version'],text=True).splitlines()[0],
        'scope':'single polling thread loopback, no TLS; attribute network uses memory storage and a separate storage worker; gateway fixture echoes without per-request entity/Lua/commits; parser views use the common runtime parser; persistent delivery is validated separately by CTest',
        'method':'one untimed warmup per benchmark command, then serial measured repetitions; summarize each metric with the median of per-run values; per-run p99 values are not pooled quantiles',
        'sha256':{},'runs':[]}
for name,path in {**executables,'libfsnet.so':build/'libfsnet.so'}.items():report['sha256'][name]=hashlib.sha256(path.read_bytes()).hexdigest()
report['runner_sha256']=hashlib.sha256(Path(__file__).read_bytes()).hexdigest()
report['cgroup_before']={name:(Path('/sys/fs/cgroup')/name).read_text().strip() for name in ('cpu.stat','memory.current') if (Path('/sys/fs/cgroup')/name).exists()}
for name,arguments in commands:
 subprocess.run([str(executables[name]),*arguments],env=environment,capture_output=True,text=True,check=True,timeout=120)
print('completed untimed warmup',flush=True)
# Run serially to avoid competing benchmark processes. Preserve all repetitions.
for repeat in range(args.runs):
 for name,arguments in commands:
  started=time.monotonic()
  result=subprocess.run([str(executables[name]),*arguments],env=environment,capture_output=True,text=True,timeout=120)
  report['runs'].append({'repeat':repeat+1,'command':[name,*arguments],'elapsed_seconds':time.monotonic()-started,'returncode':result.returncode,'stdout':result.stdout,'stderr':result.stderr})
  args.output.parent.mkdir(parents=True,exist_ok=True)
  args.output.write_text(json.dumps(report,indent=2)+'\n')
  if result.returncode:raise RuntimeError(f'{name}: {result.stderr}')
 print(f'completed repeat {repeat+1}/{args.runs}',flush=True)

report['cgroup_after']={name:(Path('/sys/fs/cgroup')/name).read_text().strip() for name in ('cpu.stat','memory.current') if (Path('/sys/fs/cgroup')/name).exists()}
args.output.write_text(json.dumps(report,indent=2)+'\n')
