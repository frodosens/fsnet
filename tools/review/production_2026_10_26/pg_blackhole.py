"""Observe a local PostgreSQL read and stop during a post-connect traffic blackhole."""

import argparse
import json
import os
from pathlib import Path
import select
import socket
import subprocess
import tempfile
import threading
import time

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--build-dir", type=Path, default=Path(".dev/build/postgres"))
parser.add_argument("--binary", type=Path, required=True)
parser.add_argument("--pg-install", type=Path)
args = parser.parse_args()
if os.geteuid() == 0:
    parser.error("initdb requires a non-root user; use the normal development account")
repo = Path(__file__).resolve().parents[3]
build = args.build_dir if args.build_dir.is_absolute() else repo / args.build_dir
installation = args.pg_install or build / "postgres-install"
pg = installation / "bin"
for name in ("initdb", "pg_ctl"):
    if not (pg / name).is_file():
        parser.error(f"missing local PostgreSQL executable: {pg / name}")
env = dict(os.environ)
env["LD_LIBRARY_PATH"] = os.pathsep.join([
    str(build), str(installation / "lib"),
    str(installation.parent / "openssl-install/lib"), env.get("LD_LIBRARY_PATH", ""),
])


def free_port():
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        return sock.getsockname()[1]


with tempfile.TemporaryDirectory(prefix="fsnet-pg-blackhole-") as folder:
    path = Path(folder)
    data = path / "db"
    port = free_port()
    pause, finish = threading.Event(), threading.Event()
    listener = socket.socket()
    listener.settimeout(10)
    listener.bind(("127.0.0.1", 0))
    listener.listen()
    proxy_port = listener.getsockname()[1]
    pipes = []

    def proxy():
        try:
            client, _ = listener.accept()
            upstream = socket.create_connection(("127.0.0.1", port), timeout=5)
            pipes.extend([client, upstream])
            while not finish.is_set():
                readable, _, _ = select.select(pipes, [], [], .05)
                for source in readable:
                    payload = source.recv(65536)
                    if not payload:
                        return
                    if not pause.is_set():
                        (upstream if source is client else client).sendall(payload)
        except OSError:
            return

    def ctl(*commands):
        subprocess.run([str(pg / "pg_ctl"), "-D", str(data), *commands],
                       check=True, env=env, capture_output=True, timeout=15)

    subprocess.run([str(pg / "initdb"), "-D", str(data), "-A", "trust",
                    "--no-locale", "-E", "UTF8"],
                   check=True, env=env, capture_output=True, timeout=15)
    with (data / "postgresql.conf").open("a") as config:
        config.write(f"\nlisten_addresses='127.0.0.1'\nport={port}\n"
                     f"unix_socket_directories='{folder}'\n")
    child = None
    try:
        ctl("-l", str(path / "server.log"), "-w", "start")
        thread = threading.Thread(target=proxy, daemon=True)
        thread.start()
        # Plaintext is limited to this disposable loopback fixture, unrelated to
        # production TLS requirements. No existing database is contacted.
        uri = f"host=127.0.0.1 port={proxy_port} dbname=postgres sslmode=disable"
        child = subprocess.Popen([str(args.binary.resolve()), uri], env=env,
                                 stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                 stderr=subprocess.PIPE, text=True)
        if not select.select([child.stdout], [], [], 10)[0]:
            raise TimeoutError("fixture database startup deadline")
        if child.stdout.readline().strip() != "ready":
            raise RuntimeError("fixture did not connect")
        pause.set()
        child.stdin.write("x")
        child.stdin.flush()
        time.sleep(.2)
        try:
            out, err = child.communicate(timeout=7)
            result = {"observation": "process_exit", "returncode": child.returncode,
                      "stdout": out, "stderr": err}
        except subprocess.TimeoutExpired:
            child.kill()
            out, err = child.communicate(timeout=3)
            result = {"observation": "external_deadline", "blocked_beyond_seconds": 7,
                      "stdout": out, "stderr": err}
    finally:
        finish.set()
        listener.close()
        if child and child.poll() is None:
            child.kill()
            child.communicate(timeout=3)
        for sock in pipes:
            sock.close()
        if (data / "postmaster.pid").exists():
            ctl("-m", "immediate", "-w", "stop")
    print(json.dumps(result))
