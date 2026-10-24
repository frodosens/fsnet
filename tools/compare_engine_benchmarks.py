#!/usr/bin/env python3
"""Alternate saved Release engine builds with matching per-build libraries."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('before',type=Path);p.add_argument('after',type=Path)
p.add_argument('--runs',type=int,default=5);p.add_argument('--output',type=Path,required=True)
p.add_argument('--quick',action='store_true')
a=p.parse_args()
if not 3<=a.runs<=20:p.error('runs must be 3..20')
commands=[('fsnet-envelope-benchmark',[])]
for size,count in ((128,20000),(4096,10000),(65536,1000)):
 for mode in ('direct','gateway'):commands.append(('fsnet-request-benchmark',[mode,str(count),str(size),'32']))
if not a.quick:commands += [('fsnet-attribute-benchmark',['all','20000']),('fsnet-container-benchmark',['all','100'])]
report={'scope':'alternating local Release samples; same fixtures, no TLS, memory storage; parser prototype before vs common borrowed parser after','builds':{},'samples':[]}
for label,path in (('before',a.before.resolve()),('after',a.after.resolve())):
 report['builds'][label]={'path':str(path),'sha256':{name:hashlib.sha256((path/name).read_bytes()).hexdigest() for name in {x[0] for x in commands}|{'libfsnet.so'}}}
def invoke(label,name,arguments):
 directory=Path(report['builds'][label]['path'])
 env=dict(os.environ);env['LD_LIBRARY_PATH']=str(directory)+((':'+env['LD_LIBRARY_PATH']) if env.get('LD_LIBRARY_PATH') else '')
 r=subprocess.run([str(directory/name),*arguments],env=env,capture_output=True,text=True,check=True,timeout=120)
 return {'build':label,'command':[name,*arguments],'rows':[json.loads(line) for line in r.stdout.splitlines()],'stderr':r.stderr}
# Untimed warmups exercise every path in both builds.
for label in ('before','after'):
 for name,arguments in commands:invoke(label,name,arguments)
for repeat in range(a.runs):
 for name,arguments in commands:
  for label in (('before','after') if repeat%2==0 else ('after','before')):
   sample=invoke(label,name,arguments);sample['repeat']=repeat+1;report['samples'].append(sample)
 a.output.parent.mkdir(parents=True,exist_ok=True);a.output.write_text(json.dumps(report,indent=2)+'\n')
 print('completed pair',repeat+1,flush=True)
