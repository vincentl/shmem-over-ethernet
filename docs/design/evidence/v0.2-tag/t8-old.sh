#!/bin/bash
# SPDX-License-Identifier: LGPL-3.0-or-later
# T8, part one, in the osoe VM: 0.1.0 (the tag) built and installed to a prefix, the README's
# ring built by that install's oshcc, and run against it on one node and two - the baseline.
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
P=/var/tmp/t8-prefix T=/var/tmp/t8-v0.1.0
exec 9>/tmp/osoe-vm.lock
flock -w 7200 9 || exit 99
rm -rf "$P" "$T" && mkdir -p "$P" "$T" && tar -xf "$HERE/tree-v0.1.0.tar" -C "$T" && cd "$T" || exit 2
cmake --preset release -DCMAKE_INSTALL_PREFIX="$P" > configure.txt 2>&1 && cmake --build build/release -j4 > build.txt 2>&1 &&
    cmake --install build/release > install.txt 2>&1 || { echo "0.1.0 did not build or install"; exit 3; }
echo "0.1.0 installed: $(ls $P/lib*/libosoe.so.* | xargs -n1 basename | tr '\n' ' ')"
"$P/bin/oshcc" -O2 -o /var/tmp/t8-ring "$HERE/ring.c" || exit 4
for map in "" "-m 0,0,1,1"; do
    echo "== 0.1.0, osoe-launch -n 4 $map:"; "$P/bin/osoe-launch" -n 4 $map -t 60 -- /var/tmp/t8-ring 2>&1 | tail -6
done
