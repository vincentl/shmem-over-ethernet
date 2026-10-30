#!/bin/bash
# SPDX-License-Identifier: LGPL-3.0-or-later
# T3's Rocky part, its four refused tests again. On Rocky Linux 9 (the osoe-rocky VM, 3.6 GiB) the
# single-node build's release and debug presets refused shmem.stat.view.many-pes, shmem.rma.L32,
# shmem.barrier.L64 and shmem.reduce.L64 by name at the /dev/shm capacity check, which counts every
# heap whole though a heap is written only as it is used. Here the same four, first as before (the
# control: refused again), then with OSOE_SHM_CAPACITY_CHECK=0, which reports the shortfall without
# refusing - whether they pass where what they write fits. On the VM's disk; the tree removed after.
#   t3-rocky-refused.sh TAR NAME
set -u
TAR=$1 NAME=$2
HERE=$(cd "$(dirname "$0")" && pwd)
TREE=/var/tmp/$NAME SUMMARY=$HERE/$NAME.summary
: > "$SUMMARY"
say() { echo "$*" | tee -a "$SUMMARY"; }
rm -rf "$TREE" && mkdir -p "$TREE" && tar -xf "$TAR" -C "$TREE" && cd "$TREE" || { say "no tree"; exit 2; }
say "== $(date -u +%FT%TZ) $NAME on $(cat /etc/redhat-release), $(uname -r); /dev/shm $(df -m /dev/shm | awk 'NR == 2 { print $4 }') MiB free, $(free -m | awk '/^Mem:/ { print $7 }') MiB available"
FOUR='^shmem\.(stat\.view\.many-pes|rma\.L32|barrier\.L64|reduce\.L64)$'
for preset in release debug; do
    cmake --preset "$preset" -DOSOE_SINGLE_NODE=ON -B "build/sn-$preset" > "configure-$preset.txt" 2>&1 &&
        cmake --build "build/sn-$preset" -j4 > "build-$preset.txt" 2>&1 || { say "$preset: did not build"; continue; }
    ctest --test-dir "build/sn-$preset" -R "$FOUR" > "test-$preset-checked.txt" 2>&1
    say "$preset, the check on: $(grep -E 'tests passed' "test-$preset-checked.txt")"
    OSOE_SHM_CAPACITY_CHECK=0 ctest --test-dir "build/sn-$preset" -R "$FOUR" -V > "test-$preset.txt" 2>&1
    say "$preset, OSOE_SHM_CAPACITY_CHECK=0: $(grep -E 'tests passed' "test-$preset.txt"); $(grep -c 'continuing, since OSOE_SHM_CAPACITY_CHECK=0' "test-$preset.txt") notices of the shortfall"
    grep -E '\(Failed\)|\(Timeout\)' "test-$preset.txt" | head -8 | tee -a "$SUMMARY"
done
say "== $(date -u +%T) done"
