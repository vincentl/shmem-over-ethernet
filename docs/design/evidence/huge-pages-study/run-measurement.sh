#!/bin/bash
# SPDX-License-Identifier: LGPL-3.0-or-later
# The huge-pages study's measurement: random-puts and osoe-bench quick on the
# release build of the branch, /dev/shm as shipped, remounted huge=advise with
# the branch's MADV_HUGEPAGE patch on, and remounted huge=within_size with no
# code; the mount restored after.
set -u
T=/var/tmp/osoe-huge/src/build/release
P=/var/tmp/osoe-huge/probe
cd $P || exit 1
$T/tools/wrapper/oshcc -O2 -o random-puts random-puts.c || exit 1
restore() { sudo -n mount -o remount,huge=never /dev/shm; }
trap restore EXIT
export OSOE_SHM_CAPACITY_CHECK=1
run_set() {
    local label=$1
    echo "=== $label: $(mount | grep ' /dev/shm ' | sed 's/.*(//') probe=${OSOE_HUGE_PAGES_PROBE:-unset} $(date +%T)"
    echo "--- random-puts, two PEs of one node, 192 MiB, 16 Mi updates, three runs"
    for r in 1 2 3; do
        $T/tools/launch/osoe-launch -n 2 -t 300 -- ./random-puts 192 16777216 2>&1 | grep -v "^$"
    done
    echo "--- random-puts, two logical nodes of one PE, 192 MiB, 2 Mi updates, two runs"
    for r in 1 2; do
        $T/tools/launch/osoe-launch -n 2 -m 0,1 -t 300 -- ./random-puts 192 2097152 2>&1 | grep -v "^$"
    done
    echo "--- osoe-bench quick, four PEs on two logical nodes"
    $T/tools/launch/osoe-launch -n 4 -m 0,0,1,1 -t 300 -- $T/tools/bench/osoe-bench quick 2>&1 | grep -v "^$"
    echo "--- meminfo after: $(grep -E 'ShmemHugePages|^Shmem:' /proc/meminfo | tr -s ' ' | tr '\n' ' ')"
}
unset OSOE_HUGE_PAGES_PROBE
sudo -n mount -o remount,huge=never /dev/shm
run_set "A. as shipped"
sudo -n mount -o remount,huge=advise /dev/shm
export OSOE_HUGE_PAGES_PROBE=1
run_set "B. huge=advise + MADV_HUGEPAGE (the patch)"
unset OSOE_HUGE_PAGES_PROBE
run_set "C. huge=advise without the patch (control)"
sudo -n mount -o remount,huge=within_size /dev/shm
run_set "D. huge=within_size, no code"
restore; trap - EXIT
echo "=== restored: $(mount | grep ' /dev/shm ')"
