#!/bin/bash
# SPDX-License-Identifier: LGPL-3.0-or-later
# T8, part two, in the osoe VM: the tree to be tagged built and installed over 0.1.0's prefix, and
# the README's ring - built against 0.1.0 in part one and not rebuilt - run against it on one node
# and two, with the new tree's launcher; the library it binds named.
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
P=/var/tmp/t8-prefix T=/var/tmp/t8-new
exec 9>/tmp/osoe-vm.lock
flock -w 10800 9 || exit 99
[ -x /var/tmp/t8-ring ] || { echo "part one's ring is gone"; exit 2; }
rm -rf "$T" && mkdir -p "$T" && tar -xf "$HERE/tree-tag.tar" -C "$T" && cd "$T" || exit 2
cmake --preset release -DCMAKE_INSTALL_PREFIX="$P" > configure.txt 2>&1 && cmake --build build/release -j4 > build.txt 2>&1 &&
    cmake --install build/release > install.txt 2>&1 || { echo "the tagged tree did not build or install"; exit 3; }
echo "installed over 0.1.0: $(ls $P/lib*/libosoe.so.* | xargs -n1 basename | tr '\n' ' ')"
echo "the ring binds: $(ldd /var/tmp/t8-ring | awk '/libosoe/ { print $1, "->", $3 }') -> $(readlink -f $(ldd /var/tmp/t8-ring | awk '/libosoe/ { print $3 }') | xargs basename)"
for map in "" "-m 0,0,1,1"; do
    echo "== the tagged library, osoe-launch -n 4 $map:"
    "$T/build/release/tools/launch/osoe-launch" -n 4 $map -t 60 -- /var/tmp/t8-ring 2>&1 | sort | tail -5
    echo "exit ${PIPESTATUS[0]}"
done
