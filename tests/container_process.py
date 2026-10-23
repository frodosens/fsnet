"""Run the shipped variable container demo with two independent Lua hosts."""
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
    'examples/lua/shared/container_entity.lua',))
with tempfile.TemporaryDirectory(prefix='fsnet-containers-') as directory:
    server_script = Path(directory) / 'server.lua'
    client_script = Path(directory) / 'client.lua'
    server_script.write_text(shared + '\n' + (root / 'examples/lua/container_entity.lua').read_text())
    client_script.write_text(shared + '\n' + (root / 'examples/lua/client/container_entity.lua').read_text())
    server = subprocess.Popen([server_exe, str(server_script), '0'], env=environment,
                              stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    try:
        assert select.select([server.stdout], [], [], 5)[0], 'server startup deadline'
        line = server.stdout.readline()
        assert line.startswith('listening '), line
        client = subprocess.run([client_exe, str(client_script), '127.0.0.1', line.split()[1]],
                                env=environment, capture_output=True, text=True, timeout=12)
        assert client.returncode == 0, client.stderr
        assert client.stdout.splitlines() == [
            'task_added\tdaily.1', 'item_added\tpotion\t1',
            'task_changed\tdaily.1', 'item_changed\tpotion\t2',
            'task_removed\tdaily.1', 'items_empty\t0',
        ], client.stdout
        print('PASS shipped dynamic containers: server grants, mutations/removals and client notification diff')
    finally:
        server.terminate()
        server.communicate(timeout=5)
