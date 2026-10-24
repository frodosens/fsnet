#!/usr/bin/env python3
"""Run Release engine baselines serially; save raw samples without production claims."""
import argparse
import hashlib
import json
from pathlib import Path
import platform
import subprocess
import time
import tempfile

parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('build',type=Path)
parser.add_argument('--delivery',action='store_true',help='also measure three independent mTLS/file-storage delivery runs')
parser.add_argument('--runs',type=int,default=3)
parser.add_argument('--output',type=Path,required=True)
args=parser.parse_args()
if not 3<=args.runs<=20:parser.error('runs must be 3..20')
build=args.build.resolve()
commands=[('fsnet-benchmark',['100000']),('fsnet-attribute-benchmark',['all','20000']),('fsnet-container-benchmark',['all','100']),('fsnet-envelope-benchmark',[])]
for size,count in ((128,20000),(4096,10000),(65536,1000)):
 for mode in ('direct','gateway'):commands.append(('fsnet-request-benchmark',[mode,str(count),str(size),'32']))
report={'platform':platform.platform(),'source_revision':subprocess.check_output(['git','rev-parse','HEAD'],text=True).strip(),
        'cpu':next((s.split(':',1)[1].strip() for s in Path('/proc/cpuinfo').read_text().splitlines() if s.startswith('model name')),''),
        'cpu_quota':Path('/sys/fs/cgroup/cpu.max').read_text().strip() if Path('/sys/fs/cgroup/cpu.max').exists() else 'unavailable',
        'scope':'single thread loopback, no TLS; attribute network uses memory storage; gateway fixture echoes without Lua/business/commits; parser views are a local review prototype, not a runtime change',
        'sha256':{},'runs':[]}
for name in {name for name,_ in commands}|{'libfsnet.so'}:report['sha256'][name]=hashlib.sha256((build/name).read_bytes()).hexdigest()
# Run serially to avoid competing benchmark processes. Preserve all repetitions.
for repeat in range(args.runs):
 for name,arguments in commands:
  started=time.monotonic()
  result=subprocess.run([str(build/name),*arguments],capture_output=True,text=True,timeout=120)
  report['runs'].append({'repeat':repeat+1,'command':[name,*arguments],'elapsed_seconds':time.monotonic()-started,'returncode':result.returncode,'stdout':result.stdout,'stderr':result.stderr})
  args.output.parent.mkdir(parents=True,exist_ok=True)
  args.output.write_text(json.dumps(report,indent=2)+'\n')
  if result.returncode:raise RuntimeError(f'{name}: {result.stderr}')
 print(f'completed repeat {repeat+1}/{args.runs}',flush=True)

if args.delivery:
 report['delivery_runs']=[]
 for repeat in range(args.runs):
  with tempfile.TemporaryDirectory(prefix='fsnet-perf-report-') as directory:
   output=Path(directory)/'report.json'
   command=['python3',str(Path(__file__).with_name('validate_delivery.py')),'--service',str(build/'fsnet-service'),'--client',str(build/'fsnet-client'),'--rounds','1000','--output',str(output)]
   subprocess.run(command,check=True,timeout=120)
   report['delivery_runs'].append(json.loads(output.read_text()))
   args.output.write_text(json.dumps(report,indent=2)+'\n')
