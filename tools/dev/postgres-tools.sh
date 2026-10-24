#!/usr/bin/env bash
# Optional Debian/Ubuntu tools for the PostgreSQL source build, without sudo.
set -euo pipefail
if [[ "${1:-}" == --help || "${1:-}" == -h ]]; then
    echo 'Usage: bash tools/dev/postgres-tools.sh'
    echo 'Uses the host signed APT sources; downloads/extracts bison, flex and m4 into .dev/root.'
    exit 0
fi
[[ $# == 0 ]] || exit 2
repo=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)
base="$repo/.dev/apt"
command -v apt-get >/dev/null
command -v dpkg-deb >/dev/null
mkdir -p "$base/lists/partial" "$base/cache/archives/partial" "$base/downloads" "$repo/.dev/root"
# Keep default host source/key locations; disable host APT hooks and isolate state.
apt_options=(-o Dir::Etc::parts=- -o Dir::Etc::main=-
    -o "Dir::State=$base" -o Dir::State::status=/var/lib/dpkg/status
    -o "Dir::State::lists=$base/lists" -o "Dir::Cache=$base/cache" -o Debug::NoLocking=true)
apt-get "${apt_options[@]}" update
cd -- "$base/downloads"
apt-get "${apt_options[@]}" download bison flex m4
for package in ./*.deb; do dpkg-deb -x "$package" "$repo/.dev/root"; done
printf 'Extracted optional tools. Source tools/dev/activate.sh again.\n'
