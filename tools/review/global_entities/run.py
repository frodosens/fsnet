#!/usr/bin/env python3
"""Record review observations, not passing regression tests (Release, loopback)."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import secrets
import select
import subprocess
import tempfile
import time


def login_reconnect(root, build, work):
    environment = dict(os.environ, FSNET_CLUSTER_TOKEN=secrets.token_hex(24),
                       FSNET_BIND_ADDRESS='127.0.0.1', FSNET_DEMO_SUBJECT='alice',
                       FSNET_STORAGE_URI='memory:', FSNET_NODE_ID='local')
    for name in ('FSNET_TLS_CA', 'FSNET_TLS_CERTIFICATE', 'FSNET_TLS_KEY',
                 'FSNET_TLS_PEER_NAME', 'FSNET_ENTITY_LEASE_MS'):
        environment.pop(name, None)
    # Same example code, but instantiate one LoginEntity locally and skip its
    # map transfer. This isolates reconnect/creation from multi-node routing.
    source = '\n'.join((root / 'examples/entities' / name).read_text()
                       for name in ('shared.lua', 'server.lua'))
    source += """
runtime.start(function()
    local entry=LoginEntity:spawn({home_node='node-b'},{
        creation_key='login-review',bootstrap_scope='review',target_node='local',
    })
    print('review ready '..entry.id)
end)
"""
    script = work / 'login-server.lua'
    script.write_text(source)
    process = subprocess.Popen([str(build / 'fsnet-script'), str(script), '0'], env=environment,
                               stdout=subprocess.PIPE, stderr=subprocess.PIPE, bufsize=0)
    try:
        buffer, port, identity = b'', None, None
        deadline = time.monotonic() + 10
        while identity is None and time.monotonic() < deadline:
            if process.poll() is not None:
                raise RuntimeError(process.stderr.read().decode())
            if not select.select([process.stdout], [], [], .1)[0]:
                continue
            buffer += os.read(process.stdout.fileno(), 4096)
            while b'\n' in buffer:
                line, buffer = buffer.split(b'\n', 1)
                text = line.decode()
                if text.startswith('listening '):
                    port = text.split()[1]
                if text.startswith('review ready '):
                    identity = text.split()[2]
        if identity is None or port is None:
            raise TimeoutError('login review startup')
        if len(identity) != 16 or any(c not in '0123456789abcdef' for c in identity):
            raise ValueError('invalid review entity ID')
        client = work / 'login-client.lua'
        client.write_text("runtime.start(function() local ok,value=pcall(function() return fsnet.ref('"
                          + identity + "'):call('login','Alice') end); print('login-result '..tostring(ok)..' '..tostring(value)) end)")
        results = []
        for _ in range(2):
            result = subprocess.run([str(build / 'fsnet-client'), str(client), '127.0.0.1', port],
                                    env=environment, text=True, capture_output=True, timeout=10, check=True)
            results.append(next(line for line in result.stdout.splitlines() if line.startswith('login-result ')))
        return {'probe': 'example_login_reconnect', 'same_subject': 'alice', 'results': results,
                'isolation': 'one local LoginEntity; home_node skips transfer'}
    finally:
        if process.poll() is None:
            process.terminate()
        try:
            _, error = process.communicate(timeout=3)
        except subprocess.TimeoutExpired:
            process.kill()
            _, error = process.communicate(timeout=3)
        if error:
            raise RuntimeError(error.decode())


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build', type=Path, default=Path('.dev/build/release'))
    parser.add_argument('--runs', type=int, default=3)
    parser.add_argument('--output', type=Path)
    args = parser.parse_args()
    if args.runs < 1:
        parser.error('--runs must be positive')
    fixtures = Path(__file__).resolve().parent
    root = fixtures.parents[2]
    build = (root / args.build).resolve()
    report = {'source_commit': subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=root, text=True).strip(),
              'platform': platform.platform(), 'build': str(build),
              'library_sha256': hashlib.sha256((build / 'libfsnet.so').read_bytes()).hexdigest(),
              'notes': ['Local mechanism comparisons; not end-to-end throughput.',
                        'Native timer clock fixed at 100000ms; index refresh uses steady clock.',
                        'Capacity 1 and control 2 model admission saturation, not production capacity.'],
              'runs': []}
    with tempfile.TemporaryDirectory(prefix='fsnet-global-review-') as directory:
        work = Path(directory)
        executable = work / 'probe'
        subprocess.run([os.environ.get('CXX', 'c++'), '-std=c++20', '-O3',
                        '-I' + str(root / 'include'), '-I' + str(root / 'src'), str(fixtures / 'probe.cpp'),
                        '-L' + str(build), '-Wl,-rpath,' + str(build), '-lfsnet', '-pthread', '-o', str(executable)],
                       cwd=root, check=True, timeout=60)
        for _ in range(args.runs):
            result = subprocess.run([str(executable)], capture_output=True, text=True, check=True, timeout=20)
            report['runs'].append([json.loads(line) for line in result.stdout.splitlines()])
        report['login_reconnect'] = login_reconnect(root, build, work)
    encoded = json.dumps(report, indent=2) + '\n'
    if args.output:
        args.output.write_text(encoded)
    else:
        print(encoded, end='')


if __name__ == '__main__':
    main()
