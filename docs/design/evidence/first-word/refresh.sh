#!/bin/bash
# SPDX-License-Identifier: LGPL-3.0-or-later
# refresh.sh TAR DIR PRESET...: a working tree's tar extracted over DIR (built
# before by setup.sh) and each preset rebuilt, under the VM's lock; a failed
# build stops before anything runs on stale binaries.
exec 9>/tmp/osoe-vm.lock; flock -n 9 || { echo "VM LOCK HELD"; exit 1; }
TAR=$1 D=$2; shift 2
cd "$D" || exit 1
tar -xf "$TAR" -C "$D" || exit 1
for p in "$@"; do
    if ! cmake --build "build/$p" -j4 > "$D.build-$p.txt" 2>&1; then
        echo "BUILD FAILED $p"; grep -E "error|warning" "$D.build-$p.txt" | head -20; exit 1
    fi
    echo "built $p, warnings $(grep -c warning: "$D.build-$p.txt")"
done
echo "REFRESH DONE $(date +%H:%M:%S)"
