# Source from Bash: . tools/dev/activate.sh
_fsnet_dev_repo=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)
export FSNET_DEV_ROOT="$_fsnet_dev_repo/.dev"
export PATH="$FSNET_DEV_ROOT/root/usr/bin:$FSNET_DEV_ROOT/python-tools/cmake/data/bin:$FSNET_DEV_ROOT/python-tools/bin:$PATH"
if [[ -d "$FSNET_DEV_ROOT/root/usr/share/bison" ]]; then
    export BISON_PKGDATADIR="$FSNET_DEV_ROOT/root/usr/share/bison"
fi
if [[ -x "$FSNET_DEV_ROOT/root/usr/bin/m4" ]]; then
    export M4="$FSNET_DEV_ROOT/root/usr/bin/m4"
fi
export FSNET_LIBRARY="$_fsnet_dev_repo/.dev/build/${FSNET_DEV_PRESET:-debug}/libfsnet.so"
unset _fsnet_dev_repo
