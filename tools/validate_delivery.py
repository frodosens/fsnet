#!/usr/bin/env python3
"""Disposable mTLS delivery acceptance: two owners, gateway, crash/handoff, load and drain."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import select
import subprocess
import tempfile
import time

parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('--service',type=Path,required=True)
parser.add_argument('--client',type=Path,required=True)
parser.add_argument('--openssl',default='openssl')
parser.add_argument('--rounds',type=int,default=100)
parser.add_argument('--output',type=Path)
args=parser.parse_args()
if not 1<=args.rounds<=10000: parser.error('rounds must be 1..10000')
processes=[]
metrics_paths=[]
peaks={}
started=time.monotonic()
line_buffers={}
def sample():
    for path in metrics_paths:
        try: value=json.loads(path.read_text())
        except (OSError,ValueError): continue
        for key in ('pending','queued_bytes','resident_bytes','lua_bytes','backend_pending'):
            if key in value: peaks[path.stem+'.'+key]=max(peaks.get(path.stem+'.'+key,0),value[key])
        assert value.get('resident_bytes',0)<=32*1024*1024,value
        assert value.get('lua_bytes',0)<=32*1024*1024,value
        if value.get('mode')=='gateway': assert value['pending']<=256 and value['queued_bytes']<=4*1024*1024,value

def line(process,timeout=15):
    deadline=time.monotonic()+timeout
    while time.monotonic()<deadline:
        sample()
        buffer=line_buffers.get(process.pid,b'')
        if b'\n' in buffer:
            value,buffer=buffer.split(b'\n',1);line_buffers[process.pid]=buffer
            return value.decode().rstrip('\r')
        if select.select([process.stdout],[],[],0.05)[0]:
            value=os.read(process.stdout.fileno(),65536)
            if not value: raise RuntimeError(f'process exited {process.poll()}: {process.stderr.read()}')
            line_buffers[process.pid]=buffer+value
    detail={path.name:path.read_text() for path in metrics_paths if path.exists()}
    process.terminate(); process.wait(timeout=5)
    raise TimeoutError(f'delivery process deadline; metrics={detail}; client={process.stderr.read()}')

def stop(process):
    if process.poll() is None: process.terminate()
    out,err=process.communicate(timeout=40)
    assert process.returncode==0,(process.returncode,out,err)

def run_client(script,environment):
    result=subprocess.run([str(args.client),str(script),'127.0.0.1',str(gateway_port)],env=environment,capture_output=True,text=True,timeout=10)
    return result

with tempfile.TemporaryDirectory(prefix='fsnet-delivery-') as directory:
    root=Path(directory)
    def openssl(*command):
        return subprocess.run([args.openssl,*command],cwd=root,check=True,stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True).stdout
    for ca in ('ca','untrusted'):
        openssl('req','-x509','-newkey','rsa:2048','-nodes','-keyout',ca+'.key','-out',ca+'.pem','-subj','/CN='+ca,'-days','1','-addext','basicConstraints=critical,CA:TRUE')
    for name in ('ownera','ownerb','gateway','alice','rogue'):
        openssl('req','-newkey','rsa:2048','-nodes','-keyout',name+'.key','-out',name+'.csr','-subj','/CN='+name+'.fsnet')
        (root/'extensions').write_text(f'subjectAltName=DNS:{name}.fsnet,IP:127.0.0.1\nextendedKeyUsage=serverAuth,clientAuth\nbasicConstraints=critical,CA:FALSE\n')
        ca='untrusted' if name=='rogue' else 'ca'
        openssl('x509','-req','-in',name+'.csr','-CA',ca+'.pem','-CAkey',ca+'.key','-CAcreateserial','-out',name+'.pem','-days','1','-extfile','extensions')
        (root/(name+'.key')).chmod(0o600)
    def fingerprint(name): return 'sha256:'+openssl('x509','-in',name+'.pem','-noout','-fingerprint','-sha256').strip().split('=',1)[1].replace(':','').lower()
    (root/'gateways.tsv').write_text(f'{fingerprint("gateway")} gateway\n')
    (root/'clients.tsv').write_text(f'{fingerprint("alice")} alice\n')
    (root/'routes.tsv').write_text('account login Actor\nentity Actor move\n')
    base=dict(os.environ,FSNET_CLUSTER_TOKEN='',FSNET_STORAGE_URI='file:'+str(root/'state'),FSNET_BIND_ADDRESS='127.0.0.1',FSNET_ENTITY_LEASE_MS='2000')
    for key in ('FSNET_DATABASE_PATH','FSNET_TLS_CA','FSNET_TLS_CERTIFICATE','FSNET_TLS_KEY','FSNET_TLS_PEER_NAME','FSNET_METRICS_FILE'): base.pop(key,None)
    def environment(name,peer='',metric=None):
        value=dict(base,FSNET_TLS_CA=str(root/'ca.pem'),FSNET_TLS_CERTIFICATE=str(root/(name+'.pem')),FSNET_TLS_KEY=str(root/(name+'.key')),FSNET_TLS_PEER_NAME=peer)
        if metric: value['FSNET_METRICS_FILE']=str(metric)
        return value
    shared="""
local Actor=fsnet.Entity:extend('Actor',{owner='id'})
Actor:attr('position',{type='vec3',client='rw',sync='observers'})
Actor:attr('payload',{type='string',default=string.rep('p',1024),sync='owner'})
Actor:attr('activations',{type='int32',default=0,sync='owner'})
Actor:attr('created',{type='int32',default=0,sync='owner'})
function Actor:on_create() self.created=self.created+1 end
function Actor:on_activate() self.activations=self.activations+1 end
Actor:rpc('move',{callers={'client'},access='owner'})
function Actor:move(data) assert(self.context.subject==self.id and self.context.caller=='client'); self.position.x=self.position.x+assert(tonumber(data)); return self.id end
function Actor:validate_write(values) return not values.position or math.abs(values.position.x)<1000000 end
Actor:define()
"""
    server_script=root/'server.lua'
    server_script.write_text(shared+"""
Actor:register()
runtime.rpc('login',{callers={'client'},handler=function(ctx,name)
    local actor=Actor:spawn_or_restore(ctx.subject)
    actor:show_to(ctx.peer);actor:possess(ctx.peer);return actor.id
end})
""")
    def owner(name,port=0):
        metric=root/(name+'.json');metrics_paths.append(metric) if metric not in metrics_paths else None
        process=subprocess.Popen([str(args.service),'owner',str(server_script),str(port),name,str(root/'gateways.tsv')],env=environment(name,metric=metric),stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True)
        processes.append(process);text=line(process);assert text.startswith('listening '),text;return process,int(text.split()[1])
    try:
        for overrides in ({'FSNET_STORAGE_URI':'memory:'},{'FSNET_TLS_CA':''}):
            invalid=dict(environment('ownera'),**overrides)
            rejected=subprocess.run([str(args.service),'owner',str(server_script),'0','ownera',str(root/'gateways.tsv')],env=invalid,capture_output=True,text=True,timeout=5)
            assert rejected.returncode!=0 and 'listening ' not in rejected.stdout,(rejected.stdout,rejected.stderr)
        a,ap=owner('ownera');b,bp=owner('ownerb')
        (root/'endpoints.tsv').write_text(f'ownera 127.0.0.1 {ap} Actor ownera.fsnet\nownerb 127.0.0.1 {bp} Actor ownerb.fsnet\n')
        gateway_metric=root/'gateway.json';metrics_paths.append(gateway_metric)
        gateway=subprocess.Popen([str(args.service),'gateway','0',str(root/'endpoints.tsv'),str(root/'clients.tsv'),str(root/'routes.tsv')],env=environment('gateway',metric=gateway_metric),stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True)
        processes.append(gateway);text=line(gateway);assert text.startswith('listening '),text;gateway_port=int(text.split()[1])
        denied=root/'denied.lua';denied.write_text("runtime.start(function() runtime.server_request('login',''); print('UNEXPECTED') end)")
        for identity in ('rogue','ownera'):
            result=run_client(denied,environment(identity,'gateway.fsnet'));assert result.returncode!=0 and 'UNEXPECTED' not in result.stdout,(identity,result.stdout,result.stderr)
        presentation="""
local entered=0
local Local={}
function Local:on_spawn() entered=entered+1 end
fsnet.entities:register(Actor,{owned=Local,observed={}})
fsnet.entities:start()
"""
        client_script=root/'client.lua'
        client_script.write_text(shared+presentation+"""
runtime.start(function()
    local id=runtime.server_request('login','Alice')
    while not fsnet.entities:get('Actor',id) do runtime.sleep(1) end
    local actor=fsnet.entities:get('Actor',id).entity
    local ref=Actor:ref(id)
    ref:move('7',{request_id='checkpoint-move',timeout_ms=3000})
    assert(actor.position.x==7 and actor.created==1)
    print('checkpoint',actor.position.x)
    while actor.activations<2 do runtime.sleep(5) end
    assert(actor.position.x==7 and actor.created==1 and entered==1)
    ref:move('7',{request_id='checkpoint-move',timeout_ms=3000}) -- replay across new owner
    assert(actor.position.x==7)
    ref:move('2',{request_id='after-crash',timeout_ms=3000})
    assert(actor.position.x==9)
    print('recovered',actor.position.x)
    for i=1,ROUNDS do actor:batch(function(draft) draft.position.x=9+i; draft.position.y=i end) end
    assert(actor.position.x==9+ROUNDS and actor.position.y==ROUNDS)
    print('load_done',ROUNDS)
    fsnet.entities:stop()
end)
""".replace('ROUNDS',str(args.rounds)))
        client=subprocess.Popen([str(args.client),str(client_script),'127.0.0.1',str(gateway_port)],env=environment('alice','gateway.fsnet'),stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True)
        processes.append(client);assert line(client)=='checkpoint\t7.0'
        deadline=time.monotonic()+5;selected=None
        while selected is None and time.monotonic()<deadline:
            sample()
            for name,process,listen in (('ownera',a,ap),('ownerb',b,bp)):
                try: metric=json.loads((root/(name+'.json')).read_text())
                except (OSError,ValueError): continue
                if metric['entities']: selected=(name,process,listen);break
            if selected is None: time.sleep(0.05)
        assert selected,'owner discovery metrics'
        name,killed,listen=selected;killed.kill();killed.communicate(timeout=5)
        recovery_start=time.monotonic();assert line(client)=='recovered\t9.0';recovery_seconds=time.monotonic()-recovery_start
        assert line(client,timeout=120)==f'load_done\t{args.rounds}'
        out,err=client.communicate(timeout=10);assert client.returncode==0,(out,err)
        # Restart killed member at the same endpoint, then gracefully drain the
        # surviving owner while a second client connection keeps its subscription.
        replacement,_=owner(name,listen)
        handoff=root/'handoff.lua'
        handoff.write_text(shared+presentation+"""
runtime.start(function()
    local id=runtime.server_request('login','Alice')
    while not fsnet.entities:get('Actor',id) do runtime.sleep(1) end
    local actor=fsnet.entities:get('Actor',id).entity
    local previous=actor.activations;local value=actor.position.x
    print('handoff_checkpoint')
    while actor.activations<=previous do runtime.sleep(5) end
    assert(actor.position.x==value and actor.created==1 and entered==1)
    print('handoff_recovered')
    fsnet.entities:stop()
end)
""")
        second=subprocess.Popen([str(args.client),str(handoff),'127.0.0.1',str(gateway_port)],env=environment('alice','gateway.fsnet'),stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True)
        processes.append(second);assert line(second)=='handoff_checkpoint'
        survivor=b if killed is a else a;stop(survivor);assert line(second)=='handoff_recovered'
        out,err=second.communicate(timeout=10);assert second.returncode==0,(out,err)
        deadline=time.monotonic()+5
        while time.monotonic()<deadline:
            sample();value=json.loads(gateway_metric.read_text())
            if value['clients']==0 and value['subscriptions']==0 and value['pending']==0 and value['queued_bytes']==0:break
            time.sleep(0.05)
        else:raise AssertionError('gateway resources did not drain')
        assert value['migrations']>=2,value
        stop(replacement);stop(gateway)
        for metric in metrics_paths:
            value=json.loads(metric.read_text())
            assert value['ready'] is False,value
            if value['mode']=='owner':assert value['entities']==0 and value['pending']==0,value
        report={'checks':['unsafe launcher configuration rejected','mutual TLS','untrusted CA rejected','unmapped certificate rejected','authenticated account delegation','automatic subscription','SIGKILL takeover with confirmed state','same RPC ID replay across owners','shared field batch load','graceful owner handoff','stable frontend entity','resource cleanup','readiness and metrics'],
                'rounds':args.rounds,'recovery_seconds':recovery_seconds,'elapsed_seconds':time.monotonic()-started,'peaks':peaks,
                'scope':'local processes, shared POSIX file storage, ephemeral PKI; not cross-machine HA or a production SLO',
                'service_sha256':hashlib.sha256(args.service.read_bytes()).hexdigest(),'client_sha256':hashlib.sha256(args.client.read_bytes()).hexdigest()}
        library=next((p for p in (args.service.parent/'libfsnet.so',args.service.parent.parent/'lib'/'libfsnet.so') if p.is_file()),None)
        if library:report['library_sha256']=hashlib.sha256(library.read_bytes()).hexdigest()
        if args.output:args.output.parent.mkdir(parents=True,exist_ok=True);args.output.write_text(json.dumps(report,indent=2)+'\n')
        print('PASS delivery: mTLS, account delegation, crash takeover, receipt replay, batch load, graceful handoff and resource cleanup')
    finally:
        for process in reversed(processes):
            if process.poll() is None:
                process.terminate()
                try:process.communicate(timeout=40)
                except subprocess.TimeoutExpired:process.kill();process.communicate(timeout=5)
