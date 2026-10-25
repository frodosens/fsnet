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
for name in ('FSNET_TLS_CA','FSNET_TLS_CERTIFICATE','FSNET_TLS_KEY','FSNET_TLS_PEER_NAME','FSNET_DATABASE_PATH','FSNET_NODE_ID','FSNET_ENTITY_LEASE_MS'):
    environment.pop(name,None)
with tempfile.TemporaryDirectory(prefix='fsnet-sdk-example-') as directory:
    shared=(root/'examples'/args.example/'shared.lua').read_text()
    server_script=Path(directory)/'server.lua';client_script=Path(directory)/'client.lua'
    server_script.write_text(shared+'\n'+(root/'examples'/args.example/'server.lua').read_text())
    client_script.write_text(shared+'\n'+(root/'examples'/args.example/'client.lua').read_text())
    server=subprocess.Popen([str(build/'fsnet-script'),str(server_script),'0'],env=environment,stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True)
    try:
        if not select.select([server.stdout],[],[],10)[0]:raise TimeoutError('example server startup')
        text=server.stdout.readline()
        if not text.startswith('listening '):raise RuntimeError('example server failed: '+server.stderr.read())
        port=text.split()[1]
        command=[str(build/'fsnet-native-client'),'127.0.0.1',port] if args.native else [str(build/'fsnet-client'),str(client_script),'127.0.0.1',port]
        subprocess.run(command,env=environment,check=True,timeout=20)
    finally:
        if server.poll() is None:server.terminate()
        try:output,error=server.communicate(timeout=5)
        except subprocess.TimeoutExpired:server.kill();output,error=server.communicate(timeout=5)
        if server.returncode not in (0,-15):raise RuntimeError(error)
