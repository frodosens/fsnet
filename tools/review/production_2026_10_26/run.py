#!/usr/bin/env python3
"""Build and record isolated review observations, not regression-test passes."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import shlex
import subprocess
import sys
import time


HERE = Path(__file__).resolve().parent
REPO = HERE.parents[2]
# (needs Lua static libraries, external observation deadline in seconds)
CPP = {
    "entity_runtime": (False, 10),
    "entity_exception": (False, 10),
    "client_activation": (False, 10),
    "client_lookup": (False, 10),
    "gateway_connection_change": (False, 10),
    "router_deadline": (False, 10),
    "lua_gc_shutdown": (True, 2),
    "lua_native_budget": (True, 15),
    "transport_poll_budget": (False, 10),
    "delivery_registry": (True, 15),
    "storage_completion_oom": (False, 10),
    "storage_memory_copy": (False, 15),
    "pg_blackhole": (False, 30),
}
PYTHON = {"token_echo": 10, "launcher": 20}
DEFAULT = [
    "entity_runtime", "entity_exception", "client_activation", "client_lookup",
    "gateway_connection_change", "router_deadline", "lua_gc_shutdown",
    "delivery_registry", "storage_completion_oom", "token_echo",
]


def text(value):
    if isinstance(value, bytes):
        return value.decode(errors="replace")
    return value or ""


def observe(command, timeout, env):
    start = time.monotonic()
    try:
        result = subprocess.run(command, cwd=REPO, env=env, capture_output=True,
                                text=True, timeout=timeout)
        return {
            "observation": "process_exit",
            "returncode": result.returncode,
            "elapsed_seconds": round(time.monotonic() - start, 3),
            "stdout": result.stdout, "stderr": result.stderr,
        }
    except subprocess.TimeoutExpired as error:
        return {
            "observation": "external_deadline",
            "deadline_seconds": timeout,
            "elapsed_seconds": round(time.monotonic() - start, 3),
            "stdout": text(error.stdout), "stderr": text(error.stderr),
        }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path, default=Path(".dev/build/release"))
    parser.add_argument("--only", nargs="+", choices=sorted(CPP.keys() | PYTHON.keys()))
    parser.add_argument("--list", action="store_true")
    parser.add_argument("--pg-install", type=Path,
                        help="optional PostgreSQL install prefix for pg_blackhole")
    args = parser.parse_args()
    if args.list:
        for name in sorted(CPP.keys() | PYTHON.keys()):
            print(name + (" (default)" if name in DEFAULT else " (explicit only)"))
        return 0

    build = args.build_dir if args.build_dir.is_absolute() else REPO / args.build_dir
    build = build.resolve()
    if not (build / "libfsnet.so").is_file():
        parser.error(f"build libfsnet.so first: {build}")
    tag = build.name + "-" + hashlib.sha256(str(build).encode()).hexdigest()[:8]
    output = REPO / ".dev/production-review/repro" / tag
    output.mkdir(parents=True, exist_ok=True)
    env = dict(os.environ)
    env["LD_LIBRARY_PATH"] = str(build) + os.pathsep + env.get("LD_LIBRARY_PATH", "")
    results = []
    build_error = False
    for name in args.only or DEFAULT:
        print(f"Observing {name}", flush=True)
        if name in CPP:
            binary = output / name
            command = shlex.split(env.get("CXX", "c++")) + [
                "-std=c++20", "-O2", f"-I{REPO}", f"-I{REPO / 'tests'}",
                f"-I{REPO / 'include'}", str(HERE / (name + ".cpp")),
            ]
            if CPP[name][0]:
                libraries = [build / "libfsnet-lua.a", build / "libfsnet-lua-vm.a"]
                if not all(library.is_file() for library in libraries):
                    parser.error(f"{name} needs a build with FSNET_WITH_LUA=ON")
                command += [str(library) for library in libraries]
            command += [f"-L{build}", f"-Wl,-rpath,{build}", "-lfsnet",
                        "-pthread", "-ldl", "-lm", "-o", str(binary)]
            compiled = observe(command, 90, env)
            if compiled.get("returncode") != 0:
                build_error = True
                results.append({"probe": name, "build": compiled})
                print(json.dumps(results[-1], ensure_ascii=False), flush=True)
                continue
            if name == "pg_blackhole":
                command = [sys.executable, str(HERE / "pg_blackhole.py"),
                           "--build-dir", str(build), "--binary", str(binary)]
                if args.pg_install:
                    command += ["--pg-install", str(args.pg_install.resolve())]
            else:
                command = [str(binary)]
            timeout = CPP[name][1]
        else:
            command = [sys.executable, str(HERE / (name + ".py")),
                       "--build-dir", str(build)]
            timeout = PYTHON[name]
        result = {"probe": name, **observe(command, timeout, env)}
        results.append(result)
        print(json.dumps(result, ensure_ascii=False), flush=True)
    destination = output / "observations.json"
    destination.write_text(json.dumps({
        "kind": "review_observations_not_test_results", "build_dir": str(build),
        "results": results,
    }, indent=2) + "\n")
    print(f"Saved observations: {destination}")
    print("Exit status reports runner/build completion; inspect observations, not a test pass count.")
    return 1 if build_error else 0


if __name__ == "__main__":
    raise SystemExit(main())
