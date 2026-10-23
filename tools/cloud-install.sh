#!/bin/bash
set -euo pipefail
base=/workspace/fsnet-env
repo=/workspace/fsnet
mkdir -p "$base"/apt/{lists/partial,archives/partial} "$base"/{root,build}
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
Dir::Cache::archives "/workspace/fsnet-env/apt/archives";
APT::Get::List-Cleanup "false";
APT
export APT_CONFIG="$base/apt/apt.conf"
/usr/bin/apt-get update
/usr/bin/apt-get -o Debug::NoLocking=1 --download-only -y install ruby ruby-dev
cd "$base/apt/archives"
/usr/bin/apt-get download ruby3.3 libruby3.3 ruby-rubygems ruby3.3-dev
for pkg in *.deb; do dpkg-deb -x "$pkg" "$base/root"; done
python3 -m pip install --target "$base/python-tools" --upgrade cmake==4.4.4 ninja==1.13.2
cat > "$base/activate-modern" <<'ACTIVATE'
export PATH=/workspace/fsnet-env/root/usr/bin:/workspace/fsnet-env/python-tools/cmake/data/bin:$PATH
export LD_LIBRARY_PATH=/workspace/fsnet-env/root/usr/lib/x86_64-linux-gnu${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}
export RUBYLIB=/workspace/fsnet-env/root/usr/lib/ruby/3.3.0:/workspace/fsnet-env/root/usr/lib/x86_64-linux-gnu/ruby/3.3.0:/workspace/fsnet-env/root/usr/lib/ruby/vendor_ruby:/workspace/fsnet-env/root/usr/lib/x86_64-linux-gnu/ruby/vendor_ruby/3.3.0
export GEM_PATH=/workspace/fsnet-env/root/usr/share/rubygems-integration/all:/workspace/fsnet-env/root/usr/lib/ruby/gems/3.3.0
export FSNET_LIBRARY=/workspace/fsnet-env/cmake-build/libfsnet.so
ACTIVATE
. "$base/activate-modern"
cmake -S "$repo" -B "$base/cmake-build" -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTING=ON
cmake --build "$base/cmake-build" -j4
ctest --test-dir "$base/cmake-build" --output-on-failure
ruby3.3 "$repo/tests/ruby_test.rb"
