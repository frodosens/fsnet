"""Execute the same binary host contract against Lua and optional Ruby processes."""
import ctypes
import os
from pathlib import Path
import secrets
import select
import subprocess
import sys
import tempfile
import time

library, lua_executable, script = sys.argv[1:]
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
environment['FSNET_LIBRARY'] = library
client = native.fsnet_create(token.encode())
assert client
children = []


def invoke(peer, route, payload):
    responses = []
    @reply_type
    def completed(status, data, length):
        responses.append((status, ctypes.string_at(data, length)))
    assert native.fsnet_call(client, peer, route, payload, len(payload), 1000, completed) == 0
    deadline = time.monotonic() + 3
    while not responses:
        assert native.fsnet_poll(client) == 0
        assert time.monotonic() < deadline
        time.sleep(.001)
    return responses[0]


def start():
    process = subprocess.Popen([lua_executable, script, '0'], env=environment,
                               stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    children.append(process)
    assert select.select([process.stdout], [], [], 5)[0], 'startup deadline'
    line = process.stdout.readline()
    assert line.startswith('listening '), line
    peer = native.fsnet_connect(client, b'127.0.0.1', int(line.split()[1]))
    assert peer > 0
    return process, peer


def message(payload):
    return b'FSE1\x00\x0242' + payload


try:
    with tempfile.TemporaryDirectory(prefix='fsnet-entity-') as directory:
        environment['FSNET_DATABASE_PATH'] = str(Path(directory) / 'state.sqlite')
        environment['FSNET_ENTITY_LEASE_MS'] = '400'
        first, a = start()
        assert invoke(a, b'counter.add', message(b'7')) == (0, b'7')
        second, b = start()
        assert invoke(b, b'counter.add', message(b'10')) == (5, b''), 'live owner must exclude the other process'
        first.kill()
        first.wait(timeout=3)
        # Takeover waits for the persisted lease, not a health-check guess.
        deadline = time.monotonic() + 3
        while True:
            response = invoke(b, b'counter.get', message(b''))
            if response[0] != 5:
                break
            assert time.monotonic() < deadline
            time.sleep(.02)
        assert response == (0, b'7'), response
        assert invoke(b, b'counter.add', message(b'2')) == (0, b'9')
        second.kill()
        second.wait(timeout=3)
        third, c = start()
        deadline = time.monotonic() + 3
        while True:
            response = invoke(c, b'counter.get', message(b''))
            if response[0] != 5:
                break
            assert time.monotonic() < deadline
            time.sleep(.02)
        assert response == (0, b'9'), response
        print('PASS entity processes: concurrent ownership exclusion, SIGKILL, lease takeover, snapshot recovery and second restart')
finally:
    native.fsnet_destroy(client)
    for process in children:
        if process.poll() is None:
            process.kill()
        process.wait(timeout=5)
