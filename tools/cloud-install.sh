#!/usr/bin/env bash
# Compatible entry point; all generated files now live under the checkout/.dev.
set -euo pipefail
repo=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
preset=${1:-debug}
case "$preset" in debug|release|asan|core|postgres) ;; *) echo 'Expected debug|release|asan|core|postgres' >&2; exit 2 ;; esac
[[ $# -le 1 ]] || exit 2
bash "$repo/tools/dev/bootstrap.sh"
export FSNET_DEV_PRESET="$preset"
. "$repo/tools/dev/activate.sh"
if [[ "$preset" == postgres ]]; then
    if ! command -v bison >/dev/null || ! command -v flex >/dev/null || ! command -v m4 >/dev/null; then
        bash "$repo/tools/dev/postgres-tools.sh"
        . "$repo/tools/dev/activate.sh"
    fi
fi
cd -- "$repo"
cmake --preset "$preset"
cmake --build --preset "$preset"
ctest --preset "$preset"
