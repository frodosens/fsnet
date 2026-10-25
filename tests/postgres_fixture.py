"""A private TLS-enabled PostgreSQL instance, real clients and crash/restart."""
import os
from pathlib import Path
import select
import socket
import subprocess
import sys
import tempfile
from urllib.parse import urlencode
from certificates import certificates

root_path, openssl, executable, library, lua, script = sys.argv[1:]
pg = Path(root_path) / 'bin'
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
    child = None
    try:
        control('-l', str(root / 'postgres.log'), '-w', 'start')
        subprocess.run([executable], env=environment, check=True, timeout=30)
        if lua != '-':
            environment['FSNET_STORAGE_URI'] = uri
            subprocess.run([sys.executable, str(Path(__file__).with_name('entity_process.py')), library, lua, script, 'postgres'], env=environment, check=True, timeout=20)
        child = subprocess.Popen([executable, '--reconnect'], env=environment, stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)
        assert select.select([child.stdout], [], [], 5)[0] and child.stdout.readline().strip() == 'ready'
        control('-m', 'immediate', '-w', 'stop')
        control('-l', str(root / 'postgres.log'), '-w', 'start')
        output, _ = child.communicate('restarted\n', timeout=10)
        assert child.returncode == 0, output
        print(output, end='')
    finally:
        if child and child.poll() is None:
            child.kill()
            child.wait(timeout=5)
        if (data / 'postmaster.pid').exists():
            control('-m', 'immediate', '-w', 'stop')
