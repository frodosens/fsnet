"""A private TLS-enabled PostgreSQL instance, real clients and crash/restart."""
import os
from pathlib import Path
import select
import socket
import subprocess
import sys
import tempfile
import threading
from contextlib import contextmanager
from urllib.parse import urlencode
from certificates import certificates

root_path, openssl, executable, lua, client = sys.argv[1:]
pg = Path(root_path) / 'bin'


@contextmanager
def network_proxy(port, replies_only=False):
    """Proxy only the disposable fixture; blackhole switches after startup."""
    listener = socket.socket()
    listener.bind(('127.0.0.1', 0))
    listener.listen()
    listener.settimeout(.05)
    paused, stopped = threading.Event(), threading.Event()
    sockets, workers = [], []

    def forward(front, back):
        try:
            while not stopped.is_set():
                readable, _, _ = select.select([front, back], [], [], .05)
                for source in readable:
                    data = source.recv(65536)
                    if not data:
                        return
                    if paused.is_set() and (not replies_only or source is back):
                        continue
                    (back if source is front else front).sendall(data)
        except OSError:
            return
        finally:
            front.close()
            back.close()

    def accept():
        while not stopped.is_set():
            try:
                front, _ = listener.accept()
            except socket.timeout:
                continue
            except OSError:
                return
            try:
                back = socket.create_connection(('127.0.0.1', port), timeout=2)
            except OSError:
                front.close()
                continue
            front.settimeout(2)
            sockets.extend([front, back])
            worker = threading.Thread(target=forward, args=(front, back), daemon=True)
            workers.append(worker)
            worker.start()

    accepter = threading.Thread(target=accept, daemon=True)
    accepter.start()
    try:
        yield listener.getsockname()[1], paused
    finally:
        stopped.set()
        listener.close()
        accepter.join(timeout=3)
        for connection in sockets:
            connection.close()
        for worker in workers:
            worker.join(timeout=3)


def read_line(child, expected, timeout):
    assert select.select([child.stdout], [], [], timeout)[0], 'child progress deadline'
    actual = child.stdout.readline().strip()
    assert actual == expected, (expected, actual)


with tempfile.TemporaryDirectory(prefix='fsnet-postgres-') as temporary:
    root = Path(temporary)
    certificates(openssl, root)
    (root / 'server.key').chmod(0o600)
    data = root / 'data'
    utility_environment = os.environ.copy()
    utility_environment['LD_LIBRARY_PATH'] = str(Path(root_path).parent / 'openssl-install' / 'lib') + ':' + str(Path(root_path) / 'lib') + ':' + utility_environment.get('LD_LIBRARY_PATH', '')
    subprocess.run([str(pg / 'initdb'), '-D', str(data), '-A', 'trust', '--no-locale', '-E', 'UTF8'], check=True, env=utility_environment, stdout=subprocess.DEVNULL)
    with socket.socket() as probe:
        probe.bind(('127.0.0.1', 0))
        port = probe.getsockname()[1]
    with (data / 'postgresql.conf').open('a') as file:
        file.write(f"\nlisten_addresses='127.0.0.1'\nport={port}\nunix_socket_directories='{temporary}'\nssl=on\nssl_cert_file='{root / 'server.pem'}'\nssl_key_file='{root / 'server.key'}'\n")
    def control(*args):
        subprocess.run([str(pg / 'pg_ctl'), '-D', str(data), *args], check=True, env=environment, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, timeout=15)
    environment = os.environ.copy()
    environment['LD_LIBRARY_PATH'] = str(Path(root_path).parent / 'openssl-install' / 'lib') + ':' + str(Path(root_path) / 'lib') + ':' + environment.get('LD_LIBRARY_PATH', '')
    # A destination supplied by libpq environment defaults must be rejected
    # before opening a socket, rather than after authentication.
    with socket.socket() as listener:
        listener.bind(('127.0.0.2', 0))
        listener.listen()
        preflight = {key: value for key, value in environment.items() if not key.startswith('PG')}
        preflight.update(PGHOST='127.0.0.2', PGPORT=str(listener.getsockname()[1]),
                         PGUSER='fsnet_dummy', PGPASSWORD='not-a-real-password')
        subprocess.run([executable, '--tls-preflight'], env=preflight, check=True, timeout=10)
        assert not select.select([listener], [], [], 0)[0], 'TLS rejection occurred after connecting'
    query = {'sslmode': 'verify-full', 'sslrootcert': str(root / 'ca.pem')}
    uri = f'postgresql://127.0.0.1:{port}/postgres?' + urlencode(query)
    environment['FSNET_TEST_POSTGRES_URI'] = uri
    query['sslrootcert'] = str(root / 'untrusted.pem')
    environment['FSNET_TEST_POSTGRES_UNTRUSTED_URI'] = f'postgresql://127.0.0.1:{port}/postgres?' + urlencode(query)
    no_pg_defaults = {key: value for key, value in environment.items() if not key.startswith('PG')}
    subprocess.run([executable, '--dns-preflight'], env=no_pg_defaults, check=True, timeout=3)
    child = None
    try:
        control('-l', str(root / 'postgres.log'), '-w', 'start')
        hostname_options = f"host=server.fsnet hostaddr=127.0.0.1 port={port} dbname=postgres sslmode=verify-full sslrootcert={root / 'ca.pem'}"
        hostname_environment = environment | {'FSNET_TEST_POSTGRES_URI': hostname_options}
        subprocess.run([executable, '--hostname'], env=hostname_environment, check=True, timeout=8)
        wrong_hostname = hostname_environment | {'FSNET_TEST_POSTGRES_URI': hostname_options.replace('host=server.fsnet ', 'host=wrong.invalid ')}
        rejected_hostname = subprocess.run([executable, '--hostname'], env=wrong_hostname, capture_output=True, text=True, timeout=8)
        assert rejected_hostname.returncode != 0, 'hostaddr bypassed TLS hostname verification'
        # localhost is deliberately mapped to numeric IPv4 loopback, rather
        # than entering a potentially blocking system DNS resolver.
        localhost = environment | {'FSNET_TEST_POSTGRES_URI':
            f'host=localhost port={port} dbname=postgres sslmode=disable'}
        subprocess.run([executable, '--hostname'], env=localhost, check=True, timeout=8)
        subprocess.run([executable], env=environment, check=True, timeout=30)
        if lua != '-':
            environment['FSNET_STORAGE_URI'] = uri
            subprocess.run([sys.executable, str(Path(__file__).with_name('timer_process.py')), lua, client, '--storage-uri', uri], env=environment, check=True, timeout=20)
        child = subprocess.Popen([executable, '--reconnect'], env=environment, stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)
        assert select.select([child.stdout], [], [], 5)[0] and child.stdout.readline().strip() == 'ready'
        control('-m', 'immediate', '-w', 'stop')
        control('-l', str(root / 'postgres.log'), '-w', 'start')
        output, _ = child.communicate('restarted\n', timeout=10)
        assert child.returncode == 0, output
        print(output, end='')
        # A TCP accept without any PostgreSQL startup response used to leave
        # blocking connect/authentication beyond application shutdown budgets.
        with socket.socket() as blackhole:
            blackhole.bind(('127.0.0.1', 0))
            blackhole.listen()
            stalled = environment | {'FSNET_TEST_POSTGRES_URI':
                f'host=127.0.0.1 port={blackhole.getsockname()[1]} dbname=postgres sslmode=disable'}
            subprocess.run([executable, '--connect-blackhole'], env=stalled, check=True, timeout=9)
        for mode, replies_only in [('--read-blackhole', False), ('--write-blackhole', True)]:
            with network_proxy(port, replies_only) as (proxy_port, paused):
                network = environment | {'FSNET_TEST_POSTGRES_URI':
                    f'host=127.0.0.1 port={proxy_port} dbname=postgres sslmode=disable'}
                child = subprocess.Popen([executable, mode], env=network, stdin=subprocess.PIPE,
                                         stdout=subprocess.PIPE, text=True)
                try:
                    read_line(child, 'ready', 8)
                    paused.set()
                    child.stdin.write('blocked\n')
                    child.stdin.flush()
                    read_line(child, 'bounded', 8)
                    paused.clear()
                    output, _ = child.communicate('recovered\n', timeout=8)
                    assert child.returncode == 0, output
                    print(output, end='')
                finally:
                    if child.poll() is None:
                        child.kill()
                        child.wait(timeout=5)
    finally:
        if child and child.poll() is None:
            child.kill()
            child.wait(timeout=5)
        if (data / 'postmaster.pid').exists():
            control('-m', 'immediate', '-w', 'stop')
