"""SIGKILL recovery of durable commands across independent source/receiver processes."""
import ctypes
from contextlib import ExitStack
import os
from pathlib import Path
import select
import signal
import socket
import subprocess
import sys
import tempfile
import time

library, executable = sys.argv[1:]
native = ctypes.CDLL(library)
reply_type = ctypes.CFUNCTYPE(None, ctypes.c_int, ctypes.c_void_p, ctypes.c_size_t)
native.fsnet_create.argtypes = [ctypes.c_char_p]
native.fsnet_create.restype = ctypes.c_void_p
native.fsnet_destroy.argtypes = [ctypes.c_void_p]
native.fsnet_connect.argtypes = [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_int]
native.fsnet_connect.restype = ctypes.c_int64
native.fsnet_poll.argtypes = [ctypes.c_void_p]
native.fsnet_call.argtypes = [ctypes.c_void_p, ctypes.c_uint64, ctypes.c_char_p,
                            ctypes.c_char_p, ctypes.c_size_t, ctypes.c_int, reply_type]
client = native.fsnet_create(b'durable')
assert client
children = []
buffers = {}
ports = []
reservations = []
for _ in range(2):
    sock = socket.socket()
    sock.bind(('127.0.0.1', 0))
    ports.append(sock.getsockname()[1])
    reservations.append(sock)


def line(process):
    deadline = time.monotonic() + 8
    while time.monotonic() < deadline:
        buffer = buffers.get(process.pid, b'')
        if b'\n' in buffer:
            value, rest = buffer.split(b'\n', 1)
            buffers[process.pid] = rest
            return value.decode()
        if select.select([process.stdout], [], [], .05)[0]:
            value = os.read(process.stdout.fileno(), 65536)
            assert value, (process.poll(), process.stderr.read())
            buffers[process.pid] = buffer + value
    raise AssertionError('child output deadline')


def start(directory, name, drop=False):
    config = dict(os.environ)
    config.pop('FSNET_TEST_DROP_ACK', None)
    if drop:
        config['FSNET_TEST_DROP_ACK'] = '1'
    process = subprocess.Popen([executable, 'server', directory, name, *map(str, ports)],
                               env=config, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    children.append(process)
    assert line(process) == f'listening {ports[name == "b"]}'
    peer = native.fsnet_connect(client, b'127.0.0.1', ports[name == 'b'])
    assert peer > 0
    return process, peer


def invoke(peer, route, payload=b''):
    responses = []
    @reply_type
    def completed(status, data, length):
        responses.append((status, ctypes.string_at(data, length)))
    assert native.fsnet_call(client, peer, route.encode(), payload, len(payload), 1000, completed) == 0
    deadline = time.monotonic() + 3
    while not responses:
        assert native.fsnet_poll(client) == 0
        assert time.monotonic() < deadline, route
        time.sleep(.001)
    return responses[0]


def entity(identifier, payload=b''):
    identifier = identifier.encode()
    return b'FSE1' + len(identifier).to_bytes(2, 'big') + identifier + payload


def eventually(peer, route, payload, expected):
    deadline = time.monotonic() + 8
    while True:
        value = invoke(peer, route, payload)
        if value == expected:
            return
        assert time.monotonic() < deadline, (route, value, expected)
        time.sleep(.02)


def kill(process):
    process.kill()
    process.communicate(timeout=5)


def stop_resources():
    # Stop every writer before deleting their shared storage. Register each
    # wait separately so one cleanup error still reaps the other children.
    with ExitStack() as reaper:
        for process in children:
            reaper.callback(process.communicate, timeout=5)
            if process.poll() is None:
                try:
                    process.kill()
                except ProcessLookupError:
                    pass  # It exited between poll and kill.
        native.fsnet_destroy(client)
        for sock in reservations:
            sock.close()


with tempfile.TemporaryDirectory(prefix='fsnet-durable-process-') as directory:
    with ExitStack() as resources:
        resources.callback(stop_resources)
        reservations[1].close()
        receiver, b = start(directory, 'b')
        assert invoke(b, 'init_target') == (0, b'ok')
        reservations[0].close()
        sender, a = start(directory, 'a')
        assert invoke(a, 'init_source') == (0, b'ok')
        # Target cannot execute while SIGSTOPped. Source returns only after
        # committing the business state and the pending command to disk.
        receiver.send_signal(signal.SIGSTOP)
        status, ticket = invoke(a, 'Source.issue', entity('alice', b'7'))
        assert status == 0 and len(ticket) == 64, (status, ticket)
        kill(sender)
        sender, a = start(directory, 'a')
        receiver.send_signal(signal.SIGCONT)
        eventually(a, 'Source.status', entity('alice', ticket), (0, b'succeeded:0:7'))
        eventually(b, 'Receiver.read', entity('bob'), (0, b'7:1'))
        assert invoke(a, 'Source.forget', entity('alice', ticket)) == (0, b'true')
        # Receiver commits the reward but deliberately withholds every ACK.
        # Killing it at that boundary must preserve its inbox along with gold.
        kill(receiver)
        receiver, b = start(directory, 'b', drop=True)
        eventually(b, 'Receiver.read', entity('bob'), (0, b'7:1'))
        status, second = invoke(a, 'Source.issue', entity('alice', b'5'))
        assert status == 0 and len(second) == 64
        assert line(receiver) == 'committed'
        kill(receiver)
        receiver, b = start(directory, 'b')
        eventually(a, 'Source.status', entity('alice', second), (0, b'succeeded:0:12'))
        for _ in range(5):
            assert invoke(b, 'Receiver.read', entity('bob')) == (0, b'12:2')
        assert invoke(a, 'Source.forget', entity('alice', second)) == (0, b'true')
        print('PASS durable processes: sender SIGKILL after accepted outbox, receiver SIGKILL after committed inbox/lost ACK, automatic recovery and no duplicate rewards')
