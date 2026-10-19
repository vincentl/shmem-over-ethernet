#!/bin/bash
# SPDX-License-Identifier: LGPL-3.0-or-later
# setup.sh TAR DIR [PRESET...]: a git archive extracted into DIR in the VM's /tmp
# and built in each preset (debug release by default), under the VM's lock.
exec 9>/tmp/osoe-vm.lock; flock -n 9 || { echo "VM LOCK HELD"; exit 1; }
TAR=$1 D=$2; shift 2
[ $# -gt 0 ] || set -- debug release
rm -rf "$D"; mkdir -p "$D"; tar -xf "$TAR" -C "$D"; cd "$D" || exit 1
# OVERLAY: a directory of files copied over the extracted tree (a scratch variant).
[ -z "$OVERLAY" ] || cp -R "$OVERLAY/." "$D/" || exit 1
for p in "$@"; do
    if ! { cmake --preset "$p" > "$D.cfg-$p.txt" 2>&1 &&
           cmake --build "build/$p" -j4 > "$D.build-$p.txt" 2>&1; }; then
        echo "BUILD FAILED $p"; tail -20 "$D.build-$p.txt"; exit 1
    fi
    echo "built $p, warnings $(grep -c warning: "$D.build-$p.txt")"
done
echo "SETUP DONE $(date +%H:%M:%S)"
