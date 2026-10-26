#!/usr/bin/env python3
"""Run a self-contained loopback SDK example, with no manual Lua concatenation."""
import argparse
import os
from pathlib import Path
import secrets
import select
import subprocess
import sys
import tempfile
import time
parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('example',choices=('scene','attributes','timers'))
parser.add_argument('--build',type=Path)
parser.add_argument('--native',action='store_true',help='use the native C++ scene client')
args=parser.parse_args()
root=Path(__file__).resolve().parent.parent
default_build=root/'.dev/build/debug' if (root/'CMakeLists.txt').is_file() else root.parent.parent/'bin'
build=(args.build or default_build).resolve()
if args.native and args.example!='scene':parser.error('--native is available for scene')
if args.example=='timers':
    subprocess.run([sys.executable,str(root/'examples/timers/run.py'),'--build',str(build)],check=True)
    sys.exit(0)
# Demonstration identity is deliberately loopback-only and single-account.
# Do not inherit a developer's production storage or TLS configuration.
environment=dict(os.environ,FSNET_CLUSTER_TOKEN=secrets.token_hex(24),FSNET_DEMO_SUBJECT='alice',FSNET_BIND_ADDRESS='127.0.0.1',FSNET_STORAGE_URI='memory:')
for name in ('FSNET_TLS_CA','FSNET_TLS_CERTIFICATE','FSNET_TLS_KEY','FSNET_TLS_PEER_NAME','FSNET_NODE_ID','FSNET_ENTITY_LEASE_MS'):
    environment.pop(name,None)
with tempfile.TemporaryDirectory(prefix='fsnet-sdk-example-') as directory:
    shared=(root/'examples'/args.example/'shared.lua').read_text()
    server_script=Path(directory)/'server.lua';client_script=Path(directory)/'client.lua'
    server_script.write_text(shared+'\n'+(root/'examples'/args.example/'server.lua').read_text())
    client_script.write_text(shared+'\n'+(root/'examples'/args.example/'client.lua').read_text())
    server=subprocess.Popen([str(build/'fsnet-script'),str(server_script),'0'],env=environment,stdout=subprocess.PIPE,stderr=subprocess.PIPE,bufsize=0)
    try:
        buffer=b'';port=None;entry=None;deadline=time.monotonic()+10
        while (port is None or entry is None) and time.monotonic()<deadline:
            if server.poll() is not None:raise RuntimeError(server.stderr.read().decode())
            if not select.select([server.stdout],[],[],.1)[0]:continue
            buffer+=os.read(server.stdout.fileno(),4096)
            while b'\n' in buffer:
                line,buffer=buffer.split(b'\n',1);line=line.decode()
                if line.startswith('listening '):port=line.split()[1]
                if line.startswith('entry '):entry=line.split()[1]
        if port is None or entry is None:raise TimeoutError('example entity bootstrap')
        if len(entry)!=16 or any(c not in '0123456789abcdef' for c in entry):raise ValueError('invalid entry EntityId')
        client_script.write_text("local entry_id='"+entry+"'\n"+client_script.read_text())
        command=[str(build/'fsnet-native-client'),'127.0.0.1',port,entry] if args.native else [str(build/'fsnet-client'),str(client_script),'127.0.0.1',port]
        subprocess.run(command,env=environment,check=True,timeout=20)
    finally:
        if server.poll() is None:server.terminate()
        try:output,error=server.communicate(timeout=5)
        except subprocess.TimeoutExpired:server.kill();output,error=server.communicate(timeout=5)
        if server.returncode not in (0,-15):raise RuntimeError(error.decode())
