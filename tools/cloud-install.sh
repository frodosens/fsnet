#!/bin/bash
set -euo pipefail
base=/workspace/fsnet-env
repo=/workspace/fsnet
mkdir -p "$base"/apt/{lists/partial,tool-archives/partial} "$base"/{root,build}
cat > "$base/apt/sources.list" <<'SOURCES'
deb [signed-by=/usr/share/keyrings/debian-archive-keyring.gpg] https://deb.debian.org/debian trixie main
deb [signed-by=/usr/share/keyrings/debian-archive-keyring.gpg] https://deb.debian.org/debian-security trixie-security main
deb [signed-by=/usr/share/keyrings/debian-archive-keyring.gpg] https://deb.debian.org/debian trixie-updates main
SOURCES
cat > "$base/apt/apt.conf" <<'APT'
Dir::Etc::parts "-";
Dir::Etc::main "-";
Dir::Etc::sourcelist "/workspace/fsnet-env/apt/sources.list";
Dir::Etc::sourceparts "-";
Dir::State::lists "/workspace/fsnet-env/apt/lists";
Dir::Cache::archives "/workspace/fsnet-env/apt/tool-archives";
APT::Get::List-Cleanup "false";
APT
export APT_CONFIG="$base/apt/apt.conf"
/usr/bin/apt-get update
/usr/bin/apt-get -o Debug::NoLocking=1 --download-only -y install bison flex m4
cd "$base/apt/tool-archives"
/usr/bin/apt-get download bison flex m4
for pkg in *.deb; do dpkg-deb -x "$pkg" "$base/root"; done
python3 -m pip install --target "$base/python-tools" --upgrade cmake==4.4.4 ninja==1.13.2
cat > "$base/activate-modern" <<'ACTIVATE'
export PATH=/workspace/fsnet-env/root/usr/bin:/workspace/fsnet-env/python-tools/cmake/data/bin:$PATH
export LD_LIBRARY_PATH=/workspace/fsnet-env/root/usr/lib/x86_64-linux-gnu${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}
export BISON_PKGDATADIR=/workspace/fsnet-env/root/usr/share/bison
export M4=/workspace/fsnet-env/root/usr/bin/m4
export FSNET_LIBRARY=/workspace/fsnet-env/cmake-build/libfsnet.so
ACTIVATE
. "$base/activate-modern"
cmake -S "$repo" -B "$base/cmake-build" -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTING=ON -DFSNET_WITH_POSTGRES=ON -DFSNET_POSTGRES_TEST_SERVER=ON
cmake --build "$base/cmake-build" -j4
ctest --test-dir "$base/cmake-build" --output-on-failure
