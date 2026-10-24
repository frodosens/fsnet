#!/usr/bin/env python3
"""Build/run bounded defect probes. Observations are not passing regression tests.

Linux only. frame/destroy use loopback TCP. --asan requires an ASAN CMake build;
--include-crash intentionally reproduces a memory error in a child process.
"""
import argparse
import json
import os
from pathlib import Path
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build', default='.dev/build/debug')
    parser.add_argument('--asan', action='store_true')
    parser.add_argument('--include-crash', action='store_true')
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[2]
    build = (root / args.build).resolve()
    with tempfile.TemporaryDirectory(prefix='fsnet-review-') as directory:
        executable = str(Path(directory) / 'probe')
        command = [os.environ.get('CXX', 'c++'), '-std=c++20', '-I' + str(root / 'include')]
        if args.asan:
            command += ['-fsanitize=address,undefined', '-fno-omit-frame-pointer']
        command += [str(root / 'tools/review/runtime_probe.cpp'),
                    str(build / 'libfsnet-lua.a'), str(build / 'libfsnet-lua-vm.a'),
                    '-L' + str(build), '-Wl,-rpath,' + str(build),
                    '-lfsnet', '-lpthread', '-ldl', '-lm', '-o', executable]
        subprocess.run(command, check=True, cwd=root)
        script = Path(directory) / 'pattern.lua'
        script.write_text("runtime.rpc('probe',function() local s=string.rep('a',100); "
                          "return tostring(s:find('a*a*a*a*b')) end)\n")
        cases = [(name, []) for name in ('policy', 'replay', 'uncertain', 'memory', 'frame')]
        cases.append(('lua', [str(script)]))
        if args.include_crash:
            cases.append(('destroy', []))
        for name, extra in cases:
            try:
                result = subprocess.run([executable, name, *extra], capture_output=True,
                                        text=True, timeout=8, cwd=root)
                # Bound sanitizer output while keeping the complete log in the caller's redirect.
                print(json.dumps({'case': name, 'returncode': result.returncode,
                                  'stdout': result.stdout, 'stderr': result.stderr}, ensure_ascii=False), flush=True)
            except subprocess.TimeoutExpired:
                print(json.dumps({'case': name, 'timeout_seconds': 8}), flush=True)


if __name__ == '__main__':
    main()
