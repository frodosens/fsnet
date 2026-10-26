#!/usr/bin/env python3
"""Validate four native SDK artifacts before creating a new GitHub SDK release."""
import argparse
import hashlib
import json
from pathlib import Path, PurePosixPath
import re
import shutil
import struct
import subprocess
import zipfile

PLATFORMS = ("linux-x86_64", "windows-x86_64", "macos-x86_64", "macos-arm64")
RUNTIMES = {"linux-x86_64": "lib/libfsnet.so", "windows-x86_64": "bin/fsnet.dll",
            "macos-x86_64": "lib/libfsnet.dylib", "macos-arm64": "lib/libfsnet.dylib"}


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


def digest(path):
    value = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            value.update(block)
    return value.hexdigest()


def gh_json(endpoint, method="GET", fields=None, absent_ok=False):
    command = ["gh", "api", endpoint, "--method", method]
    for key, value in (fields or {}).items():
        command += ["-f", key + "=" + value]
    result = subprocess.run(command, capture_output=True, text=True, timeout=60)
    if result.returncode:
        if absent_ok and "HTTP 404" in result.stderr:
            return None
        raise RuntimeError("GitHub API failed: " + result.stderr.strip())
    return json.loads(result.stdout)


def verify_run(repository, run_id, source):
    run = gh_json(f"repos/{repository}/actions/runs/{run_id}")
    require(run["status"] == "completed" and run["conclusion"] == "success", "SDK producer run did not succeed")
    require(run["head_sha"] == source, "SDK producer source revision differs from requested commit")
    require(run["path"].split("@", 1)[0] == ".github/workflows/desktop-sdk.yml", "run is not the SDK producer workflow")
    require(run["head_repository"]["full_name"] == repository, "producer run is from another repository")
    artifacts = gh_json(f"repos/{repository}/actions/runs/{run_id}/artifacts?per_page=100")["artifacts"]
    expected = {"fsnet-sdk-" + platform for platform in PLATFORMS}
    found = {artifact["name"] for artifact in artifacts if not artifact["expired"]}
    require(expected <= found, "producer run lacks one or more native SDK artifacts")
    print(f"Verified successful SDK run {run_id} at {source}")


def check_machine(data, platform):
    if platform == "linux-x86_64":
        require(data[:6] == b"\x7fELF\x02\x01" and len(data) >= 20 and
                struct.unpack_from("<H", data, 18)[0] == 62, "runtime is not an ELF x86_64 binary")
    elif platform == "windows-x86_64":
        require(data[:2] == b"MZ" and len(data) >= 64, "runtime is not a PE binary")
        offset = struct.unpack_from("<I", data, 60)[0]
        require(offset + 24 <= len(data) and data[offset:offset + 4] == b"PE\0\0", "invalid PE header")
        require(struct.unpack_from("<H", data, offset + 4)[0] == 0x8664, "runtime is not PE x86_64")
    else:
        require(data[:4] == b"\xcf\xfa\xed\xfe" and len(data) >= 8, "runtime is not a thin Mach-O 64-bit binary")
        expected = 0x0100000c if platform == "macos-arm64" else 0x01000007
        require(struct.unpack_from("<I", data, 4)[0] == expected, "Mach-O runtime architecture differs from manifest")


def safe_member(name):
    path = PurePosixPath(name)
    return bool(name) and not path.is_absolute() and ".." not in path.parts and "\\" not in name and ":" not in name


def verify_asset(path, platform, version, source):
    expected_name = f"fsnet-sdk-v{version}-{platform}.zip"
    require(path.name == expected_name, "asset basename differs from target/version")
    checksums = path.parent / "SHA256SUMS"
    require(checksums.is_file(), "artifact is missing its producer SHA256SUMS")
    lines = [line.strip().split() for line in checksums.read_text().splitlines() if line.strip()]
    require(len(lines) == 1 and len(lines[0]) == 2 and lines[0][1] == path.name and
            re.fullmatch("[a-f0-9]{64}", lines[0][0]), "invalid producer checksum record")
    checksum = digest(path)
    require(checksum == lines[0][0], "SDK ZIP checksum mismatch")
    with zipfile.ZipFile(path) as archive:
        entries = archive.infolist()
        names = [entry.filename for entry in entries]
        require(len(names) == len(set(names)), "duplicate ZIP member")
        require(all(safe_member(name) for name in names), "unsafe SDK ZIP member path")
        require(all((entry.external_attr >> 16) & 0o170000 != 0o120000 for entry in entries), "SDK ZIP contains a symlink")
        require(sum(entry.file_size for entry in entries) <= 256 * 1024 * 1024, "SDK expanded size exceeds limit")
        required = {"manifest.json", "verification.json", "include/fsnet/client.hpp",
                    "lib/cmake/fsnet/fsnetConfig.cmake", "licenses/NOTICE.txt", RUNTIMES[platform]}
        require(required <= set(names), "SDK ZIP lacks installed client files or notices")
        manifest = json.loads(archive.read("manifest.json"))
        verification = json.loads(archive.read("verification.json"))
        require(manifest.get("source_revision") == source and verification.get("source_revision") == source,
                "SDK source provenance differs across artifact/consumer proof")
        require(manifest.get("platform") == platform and manifest.get("version") == version,
                "SDK platform/version manifest mismatch")
        require(manifest.get("wire_version") == 2 and manifest.get("lua") is False and manifest.get("postgres") is False,
                "SDK is not the expected standalone core client variant")
        require(verification.get("status") == "passed" and verification.get("relocated") is True,
                "SDK consumer relocation has not passed")
        runtime_files = manifest.get("runtime_files", [])
        hashes = manifest.get("runtime_sha256", {})
        require(isinstance(runtime_files, list) and set(runtime_files) == set(hashes) and
                RUNTIMES[platform] in runtime_files, "incomplete SDK runtime manifest")
        for name in runtime_files:
            require(safe_member(name) and name in names and isinstance(hashes[name], str), "invalid runtime member")
            runtime = archive.read(name)
            require(hashlib.sha256(runtime).hexdigest() == hashes[name], "runtime SHA256 mismatch: " + name)
            check_machine(runtime, platform)
    return {"name": path.name, "sha256": checksum, "bytes": path.stat().st_size,
            "platform": platform, "runtime_files": runtime_files}


def verify_assets(artifacts, output, version, source):
    require(not output.exists() or not any(output.iterdir()), "release output directory is not empty")
    output.mkdir(parents=True, exist_ok=True)
    records = []
    for platform in PLATFORMS:
        name = f"fsnet-sdk-v{version}-{platform}.zip"
        candidates = list(artifacts.rglob(name))
        require(len(candidates) == 1, "expected exactly one SDK asset: " + name)
        record = verify_asset(candidates[0], platform, version, source)
        shutil.copyfile(candidates[0], output / name)
        records.append(record)
    (output / "SHA256SUMS").write_text("".join(record["sha256"] + "  " + record["name"] + "\n" for record in records))
    (output / "sdk-release-manifest.json").write_text(json.dumps({
        "source_revision": source, "version": version, "assets": records,
    }, indent=2) + "\n")
    return records


def publish(repository, run_id, source, version, artifacts, output):
    verify_run(repository, run_id, source)
    tag = "sdk-v" + version
    require(gh_json(f"repos/{repository}/git/ref/tags/{tag}", absent_ok=True) is None,
            "tag already exists; refusing to overwrite " + tag)
    records = verify_assets(artifacts, output, version, source)
    notes = output / "release-notes.md"
    notes.write_text(
        "Precompiled fsnet C++20 client SDK for Linux x86_64, Windows x86_64 (MinGW-w64), "
        "and macOS x86_64/arm64. No fsnet implementation sources or Lua are needed by consumers.\n\n"
        f"Source: `{source}`\n\nProducer: https://github.com/{repository}/actions/runs/{run_id}\n\n"
        "All four native runners built and executed an independent relocated SDK consumer. "
        "ZIP checksums, embedded runtime hashes, source manifests and binary architectures were verified before publishing. "
        "See each manifest for compiler/ABI requirements and bundled dependency licenses.\n")
    # Creating a missing ref is atomic: a concurrent publisher cannot silently
    # substitute another tag target between the read above and release creation.
    ref = gh_json(f"repos/{repository}/git/refs", method="POST", fields={"ref": "refs/tags/" + tag, "sha": source})
    require(ref["object"]["sha"] == source, "created tag target does not match validated source")
    assets = [output / record["name"] for record in records] + [output / "SHA256SUMS", output / "sdk-release-manifest.json"]
    subprocess.run(["gh", "release", "create", tag, "--repo", repository, "--verify-tag", "--draft",
                    "--title", "fsnet desktop client SDK " + version, "--notes-file", str(notes),
                    *map(str, assets)], check=True, timeout=300)
    release = gh_json(f"repos/{repository}/releases/tags/{tag}")
    uploaded = {asset["name"]: asset for asset in release["assets"]}
    for path in assets:
        asset = uploaded.get(path.name)
        require(asset and asset["size"] == path.stat().st_size and asset.get("digest") == "sha256:" + digest(path),
                "uploaded release asset failed server-side digest verification: " + path.name)
    subprocess.run(["gh", "release", "edit", tag, "--repo", repository, "--draft=false", "--latest=false"],
                   check=True, timeout=60)
    print(f"Published https://github.com/{repository}/releases/tag/{tag}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("action", choices=("verify-run", "verify", "publish"))
    parser.add_argument("--repository", default="frodosens/fsnet")
    parser.add_argument("--run-id", type=int)
    parser.add_argument("--source-revision", required=True)
    parser.add_argument("--version", default="0.3.1")
    parser.add_argument("--artifacts", type=Path)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    require(re.fullmatch(r"[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+", args.repository), "invalid repository")
    require(re.fullmatch(r"[a-f0-9]{40}", args.source_revision), "source_revision must be an exact commit SHA")
    require(re.fullmatch(r"[0-9]+\.[0-9]+\.[0-9]+", args.version), "invalid SDK semantic version")
    if args.action != "verify":
        require(args.run_id and args.run_id > 0, "positive producer run ID is required")
    if args.action == "verify-run":
        verify_run(args.repository, args.run_id, args.source_revision)
    elif args.action == "verify":
        require(args.artifacts and args.output, "artifacts and output are required")
        verify_assets(args.artifacts.resolve(), args.output.resolve(), args.version, args.source_revision)
    else:
        require(args.artifacts and args.output, "artifacts and output are required")
        publish(args.repository, args.run_id, args.source_revision, args.version,
                args.artifacts.resolve(), args.output.resolve())


if __name__ == "__main__":
    main()
