"""Run both shipped Lua hosts with exactly the same shared entity class."""
import os
from pathlib import Path
import secrets
import select
import subprocess
import sys
import tempfile

server_exe, client_exe, repository = sys.argv[1:]
root = Path(repository)
environment = dict(os.environ, FSNET_CLUSTER_TOKEN=secrets.token_hex(24), FSNET_DEMO_SUBJECT='alice')
shared = '\n'.join((root / path).read_text() for path in (
    'tests/fixtures/lua/shared/magic_circle.lua', 'tests/fixtures/lua/shared/circle_entity.lua'))
with tempfile.TemporaryDirectory(prefix='fsnet-shared-') as directory:
    server_script = Path(directory) / 'server.lua'
    client_script = Path(directory) / 'client.lua'
    server_script.write_text(shared + '\n' + (root / 'tests/fixtures/lua/magic_circle.lua').read_text())
    client_script.write_text(shared + '\n' + (root / 'tests/fixtures/lua/client/replicated_circle.lua').read_text())
    server = subprocess.Popen([server_exe, str(server_script), '0'], env=environment,
                              stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    try:
        assert select.select([server.stdout], [], [], 5)[0], 'server startup deadline'
        line = server.stdout.readline()
        assert line.startswith('listening '), line
        client = subprocess.run([client_exe, str(client_script), '127.0.0.1', line.split()[1]],
                                env=environment, capture_output=True, text=True, timeout=12)
        assert client.returncode == 0, client.stderr
        updates = [line.split() for line in client.stdout.splitlines() if line.startswith('circle\t')]
        assert len(updates) > 2, client.stdout
        assert float(updates[-1][1]) > float(updates[0][1]), 'server ticks did not synchronize'
        print('PASS shipped shared class: local simulation, real server ticks, Lua client replica and clean exit')
    finally:
        server.terminate()
        server.communicate(timeout=5)
