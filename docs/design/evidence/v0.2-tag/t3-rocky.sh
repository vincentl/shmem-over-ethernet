#!/bin/bash
# SPDX-License-Identifier: LGPL-3.0-or-later
# T3's Rocky part, a check of the 0.2 tag: the single-node build in its presets on Rocky Linux 9
# (the osoe-rocky VM, kernel 5.14, GCC 11), each without a warning and its suite whole, the
# models apart (they run on the host). On the VM's disk; the tree removed after.
#   t3-rocky.sh TAR NAME
set -u
TAR=$1 NAME=$2
HERE=$(cd "$(dirname "$0")" && pwd)
TREE=/var/tmp/$NAME SUMMARY=$HERE/$NAME.summary
: > "$SUMMARY"
say() { echo "$*" | tee -a "$SUMMARY"; }
rm -rf "$TREE" && mkdir -p "$TREE" && tar -xf "$TAR" -C "$TREE" && cd "$TREE" || { say "no tree"; exit 2; }
say "== $(date -u +%FT%TZ) $NAME on $(cat /etc/redhat-release), $(uname -r), $(gcc --version | head -1)"
for preset in release debug asan tsan; do
    if ! cmake --preset "$preset" -DOSOE_SINGLE_NODE=ON -B "build/sn-$preset" > "configure-$preset.txt" 2>&1; then
        say "$preset: configure failed: $(tail -2 configure-$preset.txt | tr '\n' ' ')"; continue
    fi
    if ! cmake --build "build/sn-$preset" -j4 > "build-$preset.txt" 2>&1; then
        say "$preset: build failed: $(grep -m2 'error' build-$preset.txt | tr '\n' ' ')"; continue
    fi
    say "built $preset: $(grep -c 'warning:' build-$preset.txt) warnings"
    ctest --test-dir "build/sn-$preset" -j4 -LE '^model$' > "test-$preset.txt" 2>&1
    say "$preset: $(grep -E 'tests passed' test-$preset.txt)"
    grep -E '\(Failed\)|\(Timeout\)' "test-$preset.txt" | head -10 | tee -a "$SUMMARY"
done
say "== $(date -u +%T) done"
