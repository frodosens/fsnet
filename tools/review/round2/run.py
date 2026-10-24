#!/usr/bin/env python3
"""Compile/run review observations; these are NOT passing regression tests.

Linux, local loopback only. Child exit codes and sanitizer errors are evidence,
not an assertion of healthy behavior. No service deployment/storage is used.
"""
import argparse
import json
import os
from pathlib import Path
import subprocess
import tempfile


CASES = {
    'lifecycle_admission': [], 'opaque_source': [], 'control_race': [],
    'actual_control_migration': [], 'manual_route': [], 'gateway_retention': [],
    'replica_cow': [], 'entity_burst': [], 'watch_timeout': [],
    'schema_memory': ['schema_memory.lua'], 'prediction_gc': ['prediction_gc.lua'],
    'missing_reads': ['@storage'], 'node_callback_exception': [], 'stop_race': [],
    'postgres_connect': [],
}
LUA = {'actual_control_migration', 'manual_route', 'schema_memory',
       'prediction_gc', 'watch_timeout'}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build', default='.dev/build/release')
    parser.add_argument('--case', action='append', choices=CASES)
    parser.add_argument('--tsan', action='store_true', help='enable stop_race (standalone instrumented entity host)')
    parser.add_argument('--postgres-include', type=Path, help='directory containing libpq-fe.h and postgres_ext.h')
    args = parser.parse_args()
    fixtures = Path(__file__).resolve().parent
    root = fixtures.parents[2]
    build = (root / args.build).resolve()
    selected = args.case or [name for name in CASES if name not in {'stop_race', 'postgres_connect'}]
    with tempfile.TemporaryDirectory(prefix='fsnet-review2-') as directory:
        work = Path(directory)
        for case in selected:
            executable = work / case
            if case == 'stop_race' and not args.tsan:
                print(json.dumps({'case': case, 'skipped': 'requires --tsan'}), flush=True)
                continue
            if case == 'postgres_connect' and not args.postgres_include:
                print(json.dumps({'case': case, 'skipped': 'requires --postgres-include'}), flush=True)
                continue
            command = [os.environ.get('CXX', 'c++'), '-std=c++20', '-g', '-O1', '-I' + str(root / 'include')]
            command += [str(fixtures / (case + '.cpp'))]
            if case == 'stop_race':
                command += ['-fsanitize=thread', '-fno-omit-frame-pointer', str(root / 'src/entity_host.cpp')]
            if case == 'postgres_connect':
                command += ['-DFSNET_WITH_POSTGRES', '-I' + str(args.postgres_include),
                            str(root / 'src/postgres_database.cpp'), '-l:libpq.so.5']
            if case in LUA:
                command += [str(build / 'libfsnet-lua.a'), str(build / 'libfsnet-lua-vm.a')]
            command += ['-L' + str(build), '-Wl,-rpath,' + str(build), '-lfsnet',
                        '-pthread', '-ldl', '-lm', '-o', str(executable)]
            compiled = subprocess.run(command, cwd=root, capture_output=True, text=True)
            if compiled.returncode:
                print(json.dumps({'case': case, 'phase': 'compile', 'returncode': compiled.returncode,
                                  'stderr': compiled.stderr}), flush=True)
                continue
            values = [str(work / 'records') if value == '@storage' else str(fixtures / value)
                      for value in CASES[case]]
            run = [str(executable), *values]
            if case == 'postgres_connect':
                run = ['python3', str(fixtures / 'postgres_auth_probe.py'), '--executable', str(executable)]
            try:
                result = subprocess.run(run, cwd=root, capture_output=True, text=True, timeout=15)
                print(json.dumps({'case': case, 'returncode': result.returncode,
                                  'stdout': result.stdout, 'stderr': result.stderr}), flush=True)
                if case == 'lifecycle_admission':
                    drain = subprocess.run([str(executable), 'drain'], cwd=root, capture_output=True, text=True, timeout=15)
                    print(json.dumps({'case': 'drain_admission', 'returncode': drain.returncode,
                                      'stdout': drain.stdout, 'stderr': drain.stderr}), flush=True)
            except subprocess.TimeoutExpired:
                print(json.dumps({'case': case, 'timeout_seconds': 15}), flush=True)


if __name__ == '__main__':
    main()
