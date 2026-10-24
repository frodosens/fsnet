#!/usr/bin/env bash
set -euo pipefail
repo=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)
target="$repo/.dev/python-tools"
reuse=
case "${1:-}" in
    --help|-h)
        echo 'Usage: bash tools/dev/bootstrap.sh [--reuse-tools EXISTING_PIP_TARGET]'
        echo 'Installs pinned CMake/Ninja locally; no sudo or system package changes.'
        exit 0 ;;
    --reuse-tools)
        [[ $# == 2 ]] || { echo 'Missing existing tools directory' >&2; exit 2; }
        reuse=$(cd -- "$2" && pwd) ;;
    '') [[ $# == 0 ]] || exit 2 ;;
    *) echo 'Unknown option; use --help' >&2; exit 2 ;;
esac
command -v python3 >/dev/null
if [[ -n "$reuse" && "$reuse" != "$target" ]]; then
    [[ ! -e "$target" ]] || { echo 'Target already exists; run without --reuse-tools' >&2; exit 2; }
    mkdir -p "$target"
    cp -a "$reuse/." "$target/"
fi
# Use metadata instead of importing a package from the current interpreter.
if ! python3 - "$target" "$repo/tools/dev/requirements.txt" <<'PY'
import importlib.metadata
from pathlib import Path
import sys
versions={d.metadata['Name'].lower():d.version for d in importlib.metadata.distributions(path=[sys.argv[1]])}
for row in Path(sys.argv[2]).read_text().splitlines():
    if not row or row.startswith('#'):continue
    name,version=row.split('==')
    if versions.get(name)!=version:raise SystemExit(1)
PY
then
    python3 -m pip install --upgrade --target "$target" -r "$repo/tools/dev/requirements.txt"
fi
. "$repo/tools/dev/activate.sh"
cmake --version
ninja --version
cmake --list-presets -S "$repo"
printf '\nReady. Source tools/dev/activate.sh, then cmake --preset debug.\n'
