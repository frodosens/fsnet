#!/usr/bin/env python3
"""Relocate an installed SDK and build/run a consumer without source includes."""
import argparse
import json
import os
from pathlib import Path
import shutil
import subprocess
from package import cache, windows_runtime


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--prefix", type=Path, required=True)
    parser.add_argument("--build-dir", type=Path, required=True)
    args = parser.parse_args()
    prefix = args.prefix.resolve()
    relocated = prefix.with_name(prefix.name + "-relocated")
    if relocated.exists():
        raise SystemExit(f"refusing to overwrite relocated SDK: {relocated}")
    prefix.rename(relocated)
    consumer = Path(__file__).resolve().parents[2] / "tests/sdk_consumer"
    build = args.build_dir.resolve()
    subprocess.run(["cmake", "-S", str(consumer), "-B", str(build), "-G", "Ninja",
                    "-DCMAKE_BUILD_TYPE=Release", "-DCMAKE_PREFIX_PATH=" + str(relocated)],
                   check=True, timeout=120)
    subprocess.run(["cmake", "--build", str(build), "--parallel", "2"], check=True, timeout=120)
    if os.name == "nt":
        windows_runtime(relocated, cache(build)["CMAKE_CXX_COMPILER"])
    env = dict(os.environ)
    env["PATH"] = str(relocated / "bin") + os.pathsep + env.get("PATH", "")
    env["LD_LIBRARY_PATH"] = str(relocated / "lib") + os.pathsep + env.get("LD_LIBRARY_PATH", "")
    env["DYLD_LIBRARY_PATH"] = str(relocated / "lib") + os.pathsep + env.get("DYLD_LIBRARY_PATH", "")
    binary = build / ("consumer.exe" if os.name == "nt" else "consumer")
    subprocess.run([str(binary)], env=env, check=True, timeout=20)
    revision = subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=consumer.parents[1], text=True).strip()
    (relocated / "verification.json").write_text(json.dumps({
        "source_revision": revision, "status": "passed", "relocated": True,
        "consumer": "public-client-sdk", "source_includes_required": False,
    }, indent=2) + "\n")
    print("PASS relocated installed SDK: external C++20 consumer links and runs without fsnet source includes")


if __name__ == "__main__":
    main()
