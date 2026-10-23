"""Exercise POSIX file locking and acknowledged durability across real processes."""
import ctypes
import os
from pathlib import Path
import select
import subprocess
import sys
import tempfile
import time

library, *args = sys.argv[1:]
native = ctypes.CDLL(library)
native.fsnet_store_open.argtypes = [ctypes.c_char_p]
native.fsnet_store_open.restype = ctypes.c_void_p
native.fsnet_db_close.argtypes = [ctypes.c_void_p]
native.fsnet_db_get.argtypes = [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_size_t, ctypes.POINTER(ctypes.c_size_t), ctypes.POINTER(ctypes.c_uint64)]
native.fsnet_db_get.restype = ctypes.c_void_p
native.fsnet_db_cas.argtypes = [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_size_t, ctypes.c_uint64, ctypes.c_char_p, ctypes.c_size_t]
native.fsnet_free.argtypes = [ctypes.c_void_p]


def read(store):
    size, version = ctypes.c_size_t(), ctypes.c_uint64()
    data = native.fsnet_db_get(store, b'counter', 7, ctypes.byref(size), ctypes.byref(version))
    assert data
    try:
        return int(ctypes.string_at(data, size.value)), version.value
    finally:
        native.fsnet_free(data)


if args and args[0] == 'worker':
    store = native.fsnet_store_open(('file:' + args[1]).encode())
    assert store
    rounds = int(args[2])
    for i in range(rounds):
        while True:
            value, version = read(store)
            payload = str(value + 1).encode()
            result = native.fsnet_db_cas(store, b'counter', 7, version, payload, len(payload))
            assert result >= 0
            if result:
                if rounds > 100:
                    print(value + 1, flush=True)
                break
    native.fsnet_db_close(store)
else:
    with tempfile.TemporaryDirectory(prefix='fsnet-file-process-') as directory:
        store = native.fsnet_store_open(('file:' + directory).encode())
        assert store and native.fsnet_db_cas(store, b'counter', 7, 0, b'0', 1) == 1
        children = []
        try:
            for _ in range(4):
                children.append(subprocess.Popen([sys.executable, __file__, library, 'worker', directory, '30']))
            for child in children:
                assert child.wait(timeout=10) == 0
            assert read(store)[0] == 120
            child = subprocess.Popen([sys.executable, __file__, library, 'worker', directory, '1000000'], stdout=subprocess.PIPE, text=True)
            children.append(child)
            assert select.select([child.stdout], [], [], 5)[0]
            acknowledged = int(child.stdout.readline())
            child.kill()
            child.wait(timeout=5)
            native.fsnet_db_close(store)
            store = native.fsnet_store_open(('file:' + directory).encode())
            assert read(store)[0] >= acknowledged, 'an acknowledged value was lost or corrupted'
            # A fresh writer must acquire the dead process's released lock.
            subprocess.run([sys.executable, __file__, library, 'worker', directory, '1'], check=True, timeout=5)
            assert read(store)[0] >= acknowledged + 1
            print('PASS file processes: independent CAS contenders, SIGKILL, acknowledged-value recovery and lock release')
        finally:
            native.fsnet_db_close(store)
            for child in children:
                if child.poll() is None:
                    child.kill()
                child.wait(timeout=5)
