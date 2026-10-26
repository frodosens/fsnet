#!/usr/bin/env python3
"""Run the persistent Lua timer example, including a real server crash/restart."""
import argparse
import os
from pathlib import Path
import secrets
import select
import subprocess
import tempfile
import time


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build', type=Path)
    args = parser.parse_args()
    scripts = Path(__file__).resolve().parent
    root = scripts.parent.parent
    default = root / '.dev/build/debug' if (root / 'CMakeLists.txt').is_file() else root.parent.parent / 'bin'
    build = (args.build or default).resolve()
    environment = dict(os.environ, FSNET_CLUSTER_TOKEN=secrets.token_hex(24),
                       FSNET_BIND_ADDRESS='127.0.0.1', FSNET_DEMO_SUBJECT='timer-demo',
                       FSNET_NODE_ID='local', FSNET_ENTITY_LEASE_MS='2000')
    for name in ('FSNET_TLS_CA', 'FSNET_TLS_CERTIFICATE', 'FSNET_TLS_KEY',
                 'FSNET_TLS_PEER_NAME'):
        environment.pop(name, None)
    children = []

    def start():
        server = subprocess.Popen([str(build / 'fsnet-script'), str(scripts / 'server.lua'), '0'],
                                  env=environment, stdout=subprocess.PIPE,
                                  stderr=subprocess.PIPE, bufsize=0)
        children.append(server)
        buffer, port = b'', None
        deadline = time.monotonic() + 10
        while time.monotonic() < deadline:
            if server.poll() is not None:
                raise RuntimeError(server.stderr.read().decode())
            if not select.select([server.stdout], [], [], .1)[0]:
                continue
            buffer += os.read(server.stdout.fileno(), 4096)
            while b'\n' in buffer:
                line, buffer = buffer.split(b'\n', 1)
                text = line.decode()
                if text.startswith('listening '):
                    port = text.split()[1]
                if text.startswith('timers ready '):
                    if port is None:
                        raise RuntimeError('timer demo host did not publish its port')
                    return server, port, text.split()[2]
        raise TimeoutError('timer demo host startup')

    # Stop/reap children before deleting their shared storage, including failures.
    with tempfile.TemporaryDirectory(prefix='fsnet-timer-example-') as directory:
        environment['FSNET_STORAGE_URI'] = 'file:' + str(Path(directory) / 'records')

        def client(port, identity, wait):
            # Server-generated IDs have a canonical 16-character hex encoding.
            if len(identity) != 16 or any(c not in '0123456789abcdef' for c in identity):
                raise ValueError('invalid timer entity ID')
            script = Path(directory) / 'client.lua'
            script.write_text(f"local demo_id='{identity}'\nlocal wait_for_completion={'true' if wait else 'false'}\n"
                              + (scripts / 'client.lua').read_text())
            subprocess.run([str(build / 'fsnet-client'), str(script), '127.0.0.1', port],
                           env=environment, check=True, timeout=15)

        try:
            first, port, identity = start()
            print('Created durable timer entity ' + identity, flush=True)
            client(port, identity, False)
            print('SIGKILL server; wait 4 seconds with its file storage preserved.', flush=True)
            first.kill()
            first.communicate(timeout=3)
            time.sleep(4)
            second, port, restored = start()
            if restored != identity:
                raise RuntimeError('restart created a different entity')
            print('Restarted with the same entity ' + restored, flush=True)
            client(port, restored, True)
            print('PASS: delayed/absolute tasks recovered, periodic counts completed, cancelled task stayed cancelled.', flush=True)
        finally:
            for process in children:
                if process.poll() is None:
                    process.terminate()
                try:
                    _, error = process.communicate(timeout=3)
                except subprocess.TimeoutExpired:
                    process.kill()
                    _, error = process.communicate(timeout=3)
                if error:
                    print(error.decode(), end='', flush=True)


if __name__ == '__main__':
    main()
