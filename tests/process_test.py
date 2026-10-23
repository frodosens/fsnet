"""Real process crash/restart through the public C ABI, not an open-port check."""
import ctypes
import os
import secrets
import select
import subprocess
import sys
import time

library, executable = sys.argv[1:]
native = ctypes.CDLL(library)
callback_type = ctypes.CFUNCTYPE(None, ctypes.c_int, ctypes.c_void_p, ctypes.c_size_t)
native.fsnet_create.argtypes = [ctypes.c_char_p]
native.fsnet_create.restype = ctypes.c_void_p
native.fsnet_destroy.argtypes = [ctypes.c_void_p]
native.fsnet_connect.argtypes = [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_int]
native.fsnet_connect.restype = ctypes.c_int64
native.fsnet_poll.argtypes = [ctypes.c_void_p]
native.fsnet_call.argtypes = [ctypes.c_void_p, ctypes.c_uint64, ctypes.c_char_p,
                            ctypes.c_char_p, ctypes.c_size_t, ctypes.c_int, callback_type]
token = secrets.token_hex(32)
environment = os.environ.copy()
environment['FSNET_CLUSTER_TOKEN'] = token
children = []


def start(port):
    process = subprocess.Popen([executable, str(port)], env=environment,
                               stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    children.append(process)
    if not select.select([process.stdout], [], [], 5)[0]:
        raise AssertionError('node did not announce startup')
    line = process.stdout.readline()
    if not line.startswith('listening '):
        raise AssertionError('node failed startup: ' + line)
    return process, int(line.split()[1])


handle = native.fsnet_create(token.encode())
assert handle, 'client initialization failed'


def invoke(peer, expected):
    received = []

    @callback_type
    def complete(status, data, length):
        received.append((status, ctypes.string_at(data, length)))

    data = b'binary\0payload\xff'
    assert native.fsnet_call(handle, peer, b'system.echo', data, len(data), 1000, complete) == 0
    deadline = time.monotonic() + 5
    while not received:
        assert native.fsnet_poll(handle) == 0
        assert time.monotonic() < deadline, 'callback did not complete'
        time.sleep(.001)
    assert received[0][0] == expected, received
    if expected == 0:
        assert received[0][1] == data, 'binary payload mismatch'


try:
    process, port = start(0)
    peer = native.fsnet_connect(handle, b'127.0.0.1', port)
    assert peer > 0
    invoke(peer, 0)
    process.kill()
    process.wait(timeout=5)
    invoke(peer, 4)  # connection death must resolve pending callbacks
    restarted, same_port = start(port)
    assert same_port == port
    replacement = native.fsnet_connect(handle, b'127.0.0.1', port)
    assert replacement > peer
    invoke(replacement, 0)
    print('PASS process failure: binary RPC, SIGKILL disconnect, same-port restart, fresh connection')
finally:
    native.fsnet_destroy(handle)
    for child in children:
        if child.poll() is None:
            child.kill()
        child.wait(timeout=5)
