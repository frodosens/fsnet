"""Verify the Lua process binary host contract through the native C ABI."""
import ctypes
import os
from pathlib import Path
import secrets
import select
import subprocess
import sys
import tempfile
import time

library, lua_executable = sys.argv[1:]
native = ctypes.CDLL(library)
reply_type = ctypes.CFUNCTYPE(None, ctypes.c_int, ctypes.c_void_p, ctypes.c_size_t)
handler_type = ctypes.CFUNCTYPE(None, ctypes.c_uint64, ctypes.c_void_p, ctypes.c_size_t)
native.fsnet_create.argtypes = [ctypes.c_char_p]
native.fsnet_create.restype = ctypes.c_void_p
native.fsnet_destroy.argtypes = [ctypes.c_void_p]
native.fsnet_listen.argtypes = [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_int]
native.fsnet_connect.argtypes = [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_int]
native.fsnet_connect.restype = ctypes.c_int64
native.fsnet_poll.argtypes = [ctypes.c_void_p]
native.fsnet_bind.argtypes = [ctypes.c_void_p, ctypes.c_char_p, handler_type]
native.fsnet_respond.argtypes = [ctypes.c_void_p, ctypes.c_uint64, ctypes.c_int, ctypes.c_char_p, ctypes.c_size_t]
native.fsnet_call.argtypes = [ctypes.c_void_p, ctypes.c_uint64, ctypes.c_char_p,
                            ctypes.c_char_p, ctypes.c_size_t, ctypes.c_int, reply_type]
token = secrets.token_hex(32)
environment = os.environ.copy()
environment['FSNET_CLUSTER_TOKEN'] = token
backend = native.fsnet_create(token.encode())
client = native.fsnet_create(token.encode())
assert backend and client
children = []


@handler_type
def echo(request, data, length):
    payload = ctypes.string_at(data, length)
    assert native.fsnet_respond(backend, request, 0, payload, len(payload)) == 1


@handler_type
def hang(request, data, length):
    pass


assert native.fsnet_bind(backend, b'backend.echo', echo) == 0
assert native.fsnet_bind(backend, b'backend.hang', hang) == 0
backend_port = native.fsnet_listen(backend, b'127.0.0.1', 0)
assert backend_port > 0


def invoke(peer, route, payload):
    responses = []

    @reply_type
    def completed(status, data, length):
        responses.append((status, ctypes.string_at(data, length)))

    assert native.fsnet_call(client, peer, route, payload, len(payload), 1500, completed) == 0
    deadline = time.monotonic() + 5
    while not responses:
        assert native.fsnet_poll(backend) == 0
        assert native.fsnet_poll(client) == 0
        assert time.monotonic() < deadline, 'no completion for ' + route.decode()
        time.sleep(.001)
    assert len(responses) == 1
    return responses[0]


def contract(name, command):
    process = subprocess.Popen(command, env=environment, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    children.append(process)
    assert select.select([process.stdout], [], [], 5)[0], name + ' startup deadline'
    line = process.stdout.readline()
    assert line.startswith('listening '), name + ' startup failure: ' + line
    peer = native.fsnet_connect(client, b'127.0.0.1', int(line.split()[1]))
    assert peer > 0
    binary = b'\x00game-payload\xff'
    assert invoke(peer, b'contract.echo', binary) == (0, binary)
    assert invoke(peer, b'contract.forward', binary) == (0, binary)
    assert invoke(peer, b'contract.fail', binary) == (6, b'')
    assert invoke(peer, b'contract.missing', binary) == (1, b'')
    assert invoke(peer, b'contract.timeout', binary) == (0, b'timeout')
    assert invoke(peer, b'contract.echo', binary) == (0, binary), 'error damaged the runtime'
    print('PASS ' + name + ' shared host contract: binary ownership, registration, errors, async RPC, timeout recovery')
    process.kill()
    process.wait(timeout=5)


try:
    with tempfile.TemporaryDirectory(prefix='fsnet-script-') as directory:
        script = Path(directory) / 'contract.lua'
        script.write_text('''
local peer
local function backend()
  peer = peer or runtime.connect('127.0.0.1', BACKEND_PORT)
  return peer
end
runtime.legacy_rpc('contract.echo', function(data) return data end)
runtime.legacy_rpc('contract.forward', function(data) return runtime.request(backend(), 'backend.echo', data) end)
runtime.legacy_rpc('contract.fail', function() error('intentional contract error') end)
runtime.legacy_rpc('contract.timeout', function(data)
  local response, status = runtime.await(backend(), 'backend.hang', data, 30)
  assert(status == 3 and response == '')
  return 'timeout'
end)
'''.replace('BACKEND_PORT', str(backend_port)))
        contract('Lua', [lua_executable, str(script), '0'])
finally:
    native.fsnet_destroy(client)
    native.fsnet_destroy(backend)
    for process in children:
        if process.poll() is None:
            process.kill()
        process.wait(timeout=5)
