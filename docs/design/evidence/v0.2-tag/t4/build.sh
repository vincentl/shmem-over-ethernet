#!/bin/bash
# SPDX-License-Identifier: LGPL-3.0-or-later
# build.sh: T4 of the 0.2 tag's checks - the tag tree (the git archive of main at 0edd95a9) unpacked
# into /var/tmp/t4-tree in the osoe VM and its release preset built. The caller holds the VM's lock.
#   build.sh
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
TAR=<repo>/build/scratch/tag-checks/tree-tag.tar
TREE=/var/tmp/t4-tree
rm -rf "$TREE" && mkdir -p "$TREE" && tar -xf "$TAR" -C "$TREE" && cd "$TREE" || { echo "no tree"; exit 2; }
git get-tar-commit-id < "$TAR" > "$TREE/.t4-commit" 2>/dev/null || echo "unknown" > "$TREE/.t4-commit"
echo "tree: commit $(cat .t4-commit), $(basename "$TAR")"
t0=$(date +%s)
cmake --preset release > "$HERE/build-configure.log" 2>&1 || { echo "configure failed"; exit 3; }
cmake --build build/release -j4 > "$HERE/build.log" 2>&1 || { echo "build failed"; tail -20 "$HERE/build.log"; exit 4; }
echo "built in $(( $(date +%s) - t0 )) s: $(grep -c 'warning:' "$HERE/build.log") warnings, $(grep -c 'error:' "$HERE/build.log") errors"
ls -la build/release/tools/wrapper/oshcc build/release/tools/launch/osoe-launch
build/release/tools/launch/osoe-launch --version
