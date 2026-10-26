#!/usr/bin/env python3
"""Package an installed, precompiled core SDK with provenance and notices."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import re
import shutil
import subprocess
import zipfile


PLATFORMS = ("linux-x86_64", "windows-x86_64", "macos-x86_64", "macos-arm64")
SOURCES = {
    "Asio": "https://github.com/chriskohlhoff/asio/tree/asio-1-30-2",
    "SQLiteCpp": "https://github.com/SRombauts/SQLiteCpp/tree/3.3.2",
    "SQLite": "https://github.com/SRombauts/SQLiteCpp/tree/3.3.2/sqlite3",
    "OpenSSL": "https://github.com/openssl/openssl/tree/openssl-3.5.9",
    "libjuice": "https://github.com/paullouisageneau/libjuice/tree/v1.6.2",
}


def digest(path):
    value = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            value.update(block)
    return value.hexdigest()


def cache(build):
    result = {}
    for line in (build / "CMakeCache.txt").read_text().splitlines():
        match = re.match(r"([^/#][^:]*):[^=]+=(.*)", line)
        if match:
            result[match[1]] = match[2]
    return result


def license_file(directory, *names):
    for name in names:
        found = directory / name
        if found.is_file():
            return found
    raise RuntimeError(f"required dependency license absent: {directory} ({', '.join(names)})")


def dependency_directory(build, settings, name):
    # Header-only FetchContent dependencies do not necessarily declare a
    # project(), so their *_SOURCE_DIR is not always written to CMakeCache.
    source = settings.get(name + "_SOURCE_DIR") or settings.get("FETCHCONTENT_SOURCE_DIR_" + name.upper())
    if source:
        return Path(source)
    return Path(settings.get("FETCHCONTENT_BASE_DIR", str(build / "_deps"))) / (name + "-src")


def collect_notices(prefix, build, settings, revision):
    target = prefix / "licenses"
    target.mkdir(exist_ok=True)
    dependencies = {
        "Asio-Boost-1.0.txt": license_file(dependency_directory(build, settings, "asio"), "asio/LICENSE_1_0.txt"),
        "SQLiteCpp-MIT.txt": license_file(dependency_directory(build, settings, "sqlitecpp"), "LICENSE.txt", "LICENSE"),
        "libjuice-MPL-2.0.txt": license_file(dependency_directory(build, settings, "libjuice"), "LICENSE", "LICENSE.txt"),
        "OpenSSL-Apache-2.0.txt": license_file(build / "fsnet-openssl-prefix/src/fsnet-openssl", "LICENSE.txt", "LICENSE"),
    }
    for name, source in dependencies.items():
        shutil.copyfile(source, target / name)
    (target / "SQLite-PUBLIC-DOMAIN.txt").write_text(
        "SQLite is in the public domain. SQLiteCpp bundles the SQLite amalgamation.\n"
        "Source and copyright statements: " + SOURCES["SQLite"] + "\n")
    (target / "SOURCES.json").write_text(json.dumps(SOURCES, indent=2) + "\n")
    (target / "NOTICE.txt").write_text(
        "fsnet precompiled C++ client SDK\n"
        "Upstream: https://github.com/frodosens/fsnet\n"
        f"Source revision: {revision}\n\n"
        "This binary SDK is published by the upstream project owner for use with fsnet clients.\n"
        "The adapter/plugin license does not relicense third-party code.\n"
        "Asio, SQLiteCpp, SQLite, OpenSSL and libjuice remain under their respective licenses.\n"
        "libjuice is unmodified; its complete covered source is available at the exact source URL\n"
        "in SOURCES.json, including the MPL 2.0 notice. No private dependency modifications are used.\n")


def windows_runtime(prefix, compiler):
    binary = prefix / "bin"
    binary.mkdir(exist_ok=True)
    for name in ("libstdc++-6.dll", "libgcc_s_seh-1.dll", "libwinpthread-1.dll", "libssp-0.dll"):
        located = subprocess.check_output([compiler, "-print-file-name=" + name], text=True).strip()
        source = Path(located)
        if not source.is_file():
            source = Path(compiler).parent / name
        if source.is_file():
            shutil.copyfile(source, binary / name)
        elif name != "libssp-0.dll":
            raise RuntimeError(f"MinGW runtime required but not found: {name}")
    # Compiler runtime DLLs use GCC's runtime library exception. Preserve the
    # toolchain distribution's notices alongside the SDK's dependency notices.
    toolchain = Path(compiler).resolve().parent.parent
    destination = prefix / "licenses/MinGW"
    destination.mkdir(parents=True, exist_ok=True)
    found = False
    for folder in (toolchain / "share/licenses/gcc", toolchain / "share/licenses/gcc-libs",
                   toolchain / "share/licenses/headers", toolchain / "share/licenses/crt",
                   toolchain / "share/licenses/winpthreads"):
        if folder.is_dir():
            shutil.copytree(folder, destination / folder.name, dirs_exist_ok=True)
            found = True
    (destination / "SOURCE-NOTICE.txt").write_text(
        "MinGW-w64 runtime and GCC runtime DLLs supplied by MSYS2.\n"
        "GCC: GPLv3 with GCC Runtime Library Exception 3.1; source https://gcc.gnu.org/\n"
        "MinGW-w64 headers/CRT: https://www.mingw-w64.org/\n"
        "Exact installed toolchain notices were copied when supplied by the distribution.\n"
        f"Toolchain notice directory found: {found}\n")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--prefix", type=Path, required=True)
    parser.add_argument("--build-dir", type=Path, required=True)
    parser.add_argument("--platform", choices=PLATFORMS, required=True)
    parser.add_argument("--version", default="0.3.1")
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    if not re.fullmatch(r"[0-9]+\.[0-9]+\.[0-9]+", args.version):
        parser.error("version must be a semantic numeric version")
    prefix, build = args.prefix.resolve(), args.build_dir.resolve()
    settings = cache(build)
    if settings.get("FSNET_WITH_LUA") != "OFF" or settings.get("FSNET_WITH_POSTGRES") != "OFF":
        parser.error("desktop core SDK must be built with Lua and PostgreSQL OFF")
    required = prefix / "lib/cmake/fsnet/fsnetConfig.cmake"
    if not required.is_file() or not (prefix / "include/fsnet/client.hpp").is_file():
        parser.error("prefix is not an installed fsnet SDK")
    repo = Path(__file__).resolve().parents[2]
    revision = subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=repo, text=True).strip()
    verification_path = prefix / "verification.json"
    if not verification_path.is_file():
        parser.error("run tools/sdk/verify.py before packaging")
    verification = json.loads(verification_path.read_text())
    if verification.get("source_revision") != revision or verification.get("status") != "passed":
        parser.error("SDK relocation verification does not match the current source revision")
    collect_notices(prefix, build, settings, revision)
    compiler = settings.get("CMAKE_CXX_COMPILER", "c++")
    if args.platform.startswith("windows-"):
        windows_runtime(prefix, compiler)
    runtime_files = []
    for folder in (prefix / "lib", prefix / "bin"):
        if folder.is_dir():
            for path in sorted(folder.iterdir()):
                if path.is_file() and path.suffix in (".so", ".dylib", ".dll"):
                    runtime_files.append(path.relative_to(prefix).as_posix())
    expected = {"linux-x86_64": "lib/libfsnet.so", "windows-x86_64": "bin/fsnet.dll",
                "macos-x86_64": "lib/libfsnet.dylib", "macos-arm64": "lib/libfsnet.dylib"}[args.platform]
    if expected not in runtime_files:
        raise RuntimeError("missing target runtime: " + expected)
    compiler_version = subprocess.check_output([compiler, "--version"], text=True).splitlines()[0]
    manifest = {
        "name": "fsnet-desktop-client-sdk", "version": args.version, "platform": args.platform,
        "source_revision": revision, "wire_version": 2, "lua": False, "postgres": False,
        "compiler": compiler_version, "cxx_standard": 20,
        "cxx_abi": "MinGW-w64/libstdc++" if args.platform.startswith("windows-") else
                   "AppleClang/libc++" if args.platform.startswith("macos-") else "GNU/libstdc++",
        "runtime_files": runtime_files,
        "runtime_sha256": {name: digest(prefix / name) for name in runtime_files},
        "dependency_sources": SOURCES,
        "validation": ["installed-prefix-relocated", "external-client-consumer-compiled-and-executed"],
    }
    if args.platform.startswith("linux-"):
        manifest["glibc"] = platform.libc_ver()[1]
    (prefix / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
    args.output.mkdir(parents=True, exist_ok=True)
    destination = args.output / f"fsnet-sdk-v{args.version}-{args.platform}.zip"
    with zipfile.ZipFile(destination, "w", compression=zipfile.ZIP_DEFLATED, compresslevel=9) as archive:
        for path in sorted(prefix.rglob("*")):
            if not path.is_file():
                continue
            name = path.relative_to(prefix).as_posix()
            if path.resolve().is_relative_to(prefix):
                info = zipfile.ZipInfo(name, date_time=(2026, 1, 1, 0, 0, 0))
                info.compress_type = zipfile.ZIP_DEFLATED
                info.external_attr = (0o100755 if name in runtime_files else 0o100644) << 16
                archive.writestr(info, path.read_bytes())
            else:
                raise RuntimeError("SDK contains a link outside its prefix: " + name)
    (args.output / "SHA256SUMS").write_text(digest(destination) + "  " + destination.name + "\n")
    print(destination)


if __name__ == "__main__":
    main()
