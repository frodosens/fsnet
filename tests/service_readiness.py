"""Production launcher readiness follows finite initialization and real persistence."""
import hashlib
import ctypes
import json
import os
from pathlib import Path
import signal
import select
import subprocess
import sys
import tempfile
import time

from certificates import certificates

service, openssl = map(str, sys.argv[1:3])
native = ctypes.CDLL(str(Path(service).resolve().parent / "libfsnet.so"))
callback_type = ctypes.CFUNCTYPE(None, ctypes.c_int, ctypes.c_void_p, ctypes.c_size_t)
native.fsnet_create_tls.argtypes = [ctypes.c_char_p] * 5
native.fsnet_create_tls.restype = ctypes.c_void_p
native.fsnet_connect.argtypes = [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_int]
native.fsnet_connect.restype = ctypes.c_int64
native.fsnet_call.argtypes = [ctypes.c_void_p, ctypes.c_uint64, ctypes.c_char_p,
                             ctypes.c_char_p, ctypes.c_size_t, ctypes.c_int, callback_type]
native.fsnet_poll.argtypes = [ctypes.c_void_p]
native.fsnet_destroy.argtypes = [ctypes.c_void_p]


def snapshot(path):
    try:
        return json.loads(path.read_text())
    except (OSError, json.JSONDecodeError):
        return None


with tempfile.TemporaryDirectory(prefix="fsnet-service-readiness-") as name:
    root = Path(name)
    certificates(openssl, root)
    der = subprocess.run([openssl, "x509", "-in", str(root / "client.pem"), "-outform", "DER"],
                         check=True, capture_output=True, timeout=5).stdout
    pins = root / "pins.tsv"
    pins.write_text("sha256:" + hashlib.sha256(der).hexdigest() + " fixture-gateway\n")
    env = {key: value for key, value in os.environ.items() if not key.startswith("FSNET_")}
    env.update(FSNET_TLS_CA=str(root / "ca.pem"), FSNET_TLS_CERTIFICATE=str(root / "server.pem"),
               FSNET_TLS_KEY=str(root / "server.key"), FSNET_BIND_ADDRESS="127.0.0.1")
    ports = {}

    def health(child, expected):
        if child not in ports:
            assert select.select([child.stdout], [], [], 3)[0], "service did not announce port"
            line = child.stdout.readline()
            assert line.startswith("listening "), line
            ports[child] = int(line.split()[1])
        handle = native.fsnet_create_tls(b"", str(root / "ca.pem").encode(),
                                        str(root / "client.pem").encode(), str(root / "client.key").encode(),
                                        b"server.fsnet")
        assert handle, "mTLS health client creation failed"
        results = []

        @callback_type
        def reply(status, data, size):
            results.append((status, ctypes.string_at(data, size)))

        try:
            peer = native.fsnet_connect(handle, b"127.0.0.1", ports[child])
            assert peer > 0
            assert native.fsnet_call(handle, peer, b"system.health", b"", 0, 1000, reply) == 0
            deadline = time.monotonic() + 2
            while not results:
                assert native.fsnet_poll(handle) == 0
                assert time.monotonic() < deadline, "mTLS health RPC deadline"
                time.sleep(.001)
            assert results == [expected], results
        finally:
            native.fsnet_destroy(handle)

    def launch(case, source, uri):
        script, metrics = root / (case + ".lua"), root / (case + ".json")
        script.write_text(source)
        child = subprocess.Popen([service, "owner", str(script), "0", "fixture-owner", str(pins)],
                                 env=env | {"FSNET_STORAGE_URI": uri, "FSNET_METRICS_FILE": str(metrics)},
                                 stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        return child, metrics

    def finish(child):
        if child.poll() is None:
            child.send_signal(signal.SIGTERM)
        try:
            return child.communicate(timeout=5)
        except subprocess.TimeoutExpired:
            child.kill()
            child.communicate(timeout=2)
            raise AssertionError("service did not stop within test deadline")

    for index, uri in enumerate(("memory:", "sqlite::memory:", "sqlite:")):
        child, _ = launch("volatile-" + str(index), "", uri)
        try:
            out, err = child.communicate(timeout=5)
            assert child.returncode != 0 and "persistent" in err and "listening" not in out, (uri, out, err)
        finally:
            finish(child)

    for case, source in (("delayed", "runtime.start(function() runtime.sleep(400) end)"),
                         ("no-start", "")):
        child, metrics = launch(case, source, "file:" + str(root / case))
        states = []
        checked_pending = False
        try:
            deadline = time.monotonic() + 5
            while time.monotonic() < deadline:
                assert child.poll() is None, "healthy initialization unexpectedly exited"
                state = snapshot(metrics)
                if state:
                    states.append(state)
                    if case == "delayed" and state["lua_active"] and not state["ready"] and not checked_pending:
                        health(child, (2, b"draining"))
                        checked_pending = True
                    if state["ready"]:
                        break
                time.sleep(.01)
            assert states and states[-1]["ready"] and states[-1]["startup_complete"], states
            assert not states[-1]["startup_failed"], states[-1]
            if case == "delayed":
                assert checked_pending, states
            health(child, (0, b"ready"))
            out, err = finish(child)
            assert child.returncode == 0, (out, err)
            assert snapshot(metrics)["ready"] is False
        finally:
            finish(child)

    child, metrics = launch("failed", 'runtime.start(function() runtime.sleep(30); error("bootstrap fixture failure") end)',
                            "file:" + str(root / "failed"))
    try:
        out, err = child.communicate(timeout=5)
        state = snapshot(metrics)
        assert child.returncode != 0 and "initialization failed" in err, (out, err)
        assert state and not state["ready"] and state["startup_failed"] and not state["startup_complete"], state
    finally:
        finish(child)

    child, metrics = launch("failed-during-drain", 'runtime.start(function() runtime.sleep(100); error("failure during drain") end)',
                            "file:" + str(root / "failed-during-drain"))
    try:
        deadline = time.monotonic() + 3
        while time.monotonic() < deadline:
            state = snapshot(metrics)
            if state and state["lua_active"]:
                break
            assert child.poll() is None, "initialization failed before shutdown interleave"
            time.sleep(.005)
        assert state and state["lua_active"] and not state["ready"], state
        out, err = finish(child)
        state = snapshot(metrics)
        assert child.returncode != 0 and "initialization failed" in err, (out, err)
        assert state and state["startup_failed"] and not state["ready"], state
    finally:
        finish(child)

print("PASS service: durable storage required; initialization pending, success and failure control readiness")
