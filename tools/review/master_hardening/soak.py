#!/usr/bin/env python3
"""Run a local Lua/Gateway/file-CAS soak and sample process resources once/second."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import tempfile
import time

p=argparse.ArgumentParser(description=__doc__)
p.add_argument('build',type=Path)
p.add_argument('--seconds',type=int,default=300)
p.add_argument('--output',type=Path,required=True)
a=p.parse_args()
if not 1<=a.seconds<=1800:p.error('seconds must be 1..1800')
root=Path(__file__).resolve().parents[3];build=a.build.resolve()
a.output.parent.mkdir(parents=True,exist_ok=True)
report={'scope':'single process, two owners + Gateway + Client + Lua, loopback TCP, POSIX file CAS, 8 concurrent move RPCs; no TLS; 4096 rolling latency samples',
        'seconds_requested':a.seconds,'sample_interval_seconds':1,'cpu_max':Path('/sys/fs/cgroup/cpu.max').read_text().strip(),
        'memory_max':Path('/sys/fs/cgroup/memory.max').read_text().strip(),
        'sha256':{n:hashlib.sha256((build/n).read_bytes()).hexdigest() for n in ('libfsnet.so','fsnet-global-flow-tests')},'samples':[]}
with tempfile.TemporaryDirectory(prefix='fsnet-soak-') as directory:
    temporary=Path(directory)
    report['storage_filesystem']=subprocess.check_output(['stat','-f','-c','%T',directory],text=True).strip()
    command=[str(build/'fsnet-global-flow-tests'),str(root),'--soak',str(a.seconds),'file:'+str(temporary/'state')]
    report['command']=command
    with (temporary/'stdout').open('w+') as out,(temporary/'stderr').open('w+') as err:
        start=time.monotonic();process=subprocess.Popen(command,stdout=out,stderr=err)
        try:
            while process.poll() is None:
                if time.monotonic()-start>a.seconds+45:raise TimeoutError('soak exceeded bounded deadline')
                try:
                    status=dict(line.split(':',1) for line in Path(f'/proc/{process.pid}/status').read_text().splitlines())
                    stat=Path(f'/proc/{process.pid}/stat').read_text().split()
                    report['samples'].append({'seconds':time.monotonic()-start,'rss_kib':int(status.get('VmRSS','0 kB').split()[0]),
                      'peak_rss_kib':int(status.get('VmHWM','0 kB').split()[0]),'threads':int(status['Threads']),
                      'fds':len(list(Path(f'/proc/{process.pid}/fd').iterdir())),
                      'cpu_seconds':(int(stat[13])+int(stat[14]))/os.sysconf('SC_CLK_TCK')})
                except FileNotFoundError:pass
                a.output.write_text(json.dumps(report,indent=2)+'\n')
                time.sleep(1)
            report['returncode']=process.returncode
            report['elapsed_seconds']=time.monotonic()-start
        finally:
            if process.poll() is None:process.kill()
            process.wait();out.seek(0);err.seek(0)
            report['stdout']=out.read();report['stderr']=err.read()
            report['storage_files']=sum(1 for f in (temporary/'state').rglob('*') if f.is_file())
            report['storage_bytes']=sum(f.stat().st_size for f in (temporary/'state').rglob('*') if f.is_file())
            a.output.write_text(json.dumps(report,indent=2)+'\n')
    if process.returncode:raise SystemExit(process.returncode)
print('soak passed',report['elapsed_seconds'],flush=True)
