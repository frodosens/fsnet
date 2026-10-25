"""Kill/restart the real Lua host; recover delayed and periodic entity timers."""
import os
import argparse
from pathlib import Path
import secrets
import select
import subprocess
import sys
import tempfile
import time

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("server")
parser.add_argument("client")
parser.add_argument("--storage-uri")
args = parser.parse_args()
server_exe, client_exe = args.server, args.client
children = []
environment = dict(os.environ, FSNET_CLUSTER_TOKEN=secrets.token_hex(24),
                   FSNET_BIND_ADDRESS='127.0.0.1', FSNET_DEMO_SUBJECT='timer-client',
                   FSNET_ENTITY_LEASE_MS='500', FSNET_NODE_ID='local')
for name in ('FSNET_TLS_CA', 'FSNET_TLS_CERTIFICATE', 'FSNET_TLS_KEY', 'FSNET_TLS_PEER_NAME', 'FSNET_DATABASE_PATH'):
    environment.pop(name, None)

source = """
local Clock=fsnet.Entity:extend('Clock')
Clock:attr('ready',{type='bool',default=false})
Clock:attr('once',{type='int32',default=0})
Clock:attr('periodic',{type='int32',default=0})
Clock:attr('cancelled',{type='int32',default=0})
function Clock:one_shot() self.once=self.once+1 end
function Clock:repeat_tick() self.periodic=self.periodic+1 end
function Clock:should_never_fire() self.cancelled=self.cancelled+1 end
Clock:rpc('ensure',{callers={'server'}})
function Clock:ensure()
    if self.ready then return end
    self:after(1200,'one_shot','',{key='once'})
    self:every(500,'repeat_tick','',{key='periodic',count=3})
    self:after(250,'should_never_fire','',{key='cancelled'})
    assert(self:cancel_timer('cancelled'))
    self.ready=true
end
Clock:rpc('read',{callers={'client'}})
function Clock:read() return self.once..':'..self.periodic..':'..self.cancelled end
Clock:register()
runtime.start(function()
    local actor=Clock:spawn({},{creation_key='clock',bootstrap_scope='TIMER_SCOPE',target_node='local'})
    -- A killed owner retains its lease briefly. Bootstrap retries this
    -- idempotent operation while the timer worker observes the same fence.
    local ready=false
    for attempt=1,100 do
        if pcall(function() actor:ensure('') end) then ready=true;break end
        runtime.sleep(20)
    end
    assert(ready,'bootstrap lease recovery deadline')
    print('ready '..actor.id)
end)
"""
source = source.replace("TIMER_SCOPE", "timer-process-" + secrets.token_hex(8))


def start(script):
    process = subprocess.Popen([server_exe, str(script), '0'], env=environment,
                               stdout=subprocess.PIPE, stderr=subprocess.PIPE, bufsize=0)
    children.append(process)
    buffer = b''
    port = None
    end = time.monotonic() + 8
    while time.monotonic() < end:
        if process.poll() is not None:
            raise RuntimeError(process.stderr.read().decode())
        if not select.select([process.stdout], [], [], .1)[0]:
            continue
        buffer += os.read(process.stdout.fileno(), 4096)
        while b'\n' in buffer:
            line, buffer = buffer.split(b'\n', 1)
            text = line.decode()
            if text.startswith('listening '):
                port = text.split()[1]
            if text.startswith('ready '):
                assert port is not None
                return process, port, text.split()[1]
    raise TimeoutError('timer host startup')


def read(script, port, identity):
    script.write_text("runtime.start(function() print('state '..fsnet.ref('" + identity + "'):call('read','')) end)")
    result = subprocess.run([client_exe, str(script), '127.0.0.1', port], env=environment,
                            text=True, capture_output=True, timeout=5, check=True)
    line = next(line for line in result.stdout.splitlines() if line.startswith('state '))
    return tuple(map(int, line.split()[1].split(':')))


try:
    with tempfile.TemporaryDirectory(prefix='fsnet-timer-process-') as directory:
        root = Path(directory)
        environment['FSNET_STORAGE_URI'] = args.storage_uri or 'file:' + str(root / 'records')
        script, query = root / 'server.lua', root / 'client.lua'
        script.write_text(source)
        first, port, identity = start(script)
        first.kill()
        first.wait(timeout=3)
        time.sleep(1.8)  # past the original delay, periods and old owner lease
        second, port, restored = start(script)
        assert restored == identity, (identity, restored)
        end = time.monotonic() + 5
        while True:
            state = read(query, port, identity)
            assert state[0] <= 1 and state[1] <= 3 and state[2] == 0, state
            if state == (1, 3, 0):
                break
            assert time.monotonic() < end, state
            time.sleep(.05)
        second.kill()
        second.wait(timeout=3)
        third, port, restored = start(script)
        assert restored == identity
        assert read(query, port, identity) == (1, 3, 0)
        time.sleep(.3)
        assert read(query, port, identity) == (1, 3, 0)
        print('PASS timer process: SIGKILL, persistent delay and periodic count, cancellation, stable ID and no refire after second restart')
finally:
    for process in children:
        if process.poll() is None:
            process.terminate()
        try:
            process.communicate(timeout=3)
        except subprocess.TimeoutExpired:
            process.kill()
            process.communicate(timeout=3)
