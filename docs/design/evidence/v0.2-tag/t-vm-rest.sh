#!/bin/bash
# SPDX-License-Identifier: LGPL-3.0-or-later
# The 0.2 tag's last runs in the osoe VM, on T1's tree (/var/tmp/t1, from tree-tag.tar, built by
# t1.sh): T1's suite under load again - t1.sh passed ctest's --timeout where its run() takes an
# environment, so env refused it and nothing ran - eight busy loops beside the release suite at the
# default helpers; and T6's VM half, the floor's pins (OSOE_TEST_FLOOR_PINS=1) on shmem.floor three
# times, and on tier 3's floor tests configured with -DOSOE_TEST_PINS=ON, as the project's runs do.
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
TREE=/var/tmp/t1 OUT=/var/tmp/t1-out SUMMARY=$HERE/t-vm-rest.summary
exec 9>/tmp/osoe-vm.lock
flock -w 10800 9 || { echo "exit 99: no lock" >> "$SUMMARY"; exit 1; }
say() { echo "$*" | tee -a "$SUMMARY"; }
: > "$SUMMARY"
cd "$TREE" || { say "no tree"; exit 2; }
say "== $(date -u +%FT%TZ) on $TREE (tree-tag.tar); load $(cut -d' ' -f1-3 /proc/loadavg)"
pids=(); for i in 1 2 3 4 5 6 7 8; do (while :; do :; done) & pids+=($!); done
say "-- $(date -u +%T) release under load, eight busy loops"
ctest --test-dir build/release -j4 -LE '^model$' --timeout 600 > "$OUT/release-under-load.txt" 2>&1
kill "${pids[@]}" 2>/dev/null; wait 2>/dev/null
say "release-under-load: $(grep -E 'tests passed' "$OUT/release-under-load.txt")"
grep -E '\(Failed\)|\(Timeout\)|\*\*\*Exception' "$OUT/release-under-load.txt" | head -12 | tee -a "$SUMMARY"
say "-- $(date -u +%T) the floor's pins, load $(cut -d' ' -f1-3 /proc/loadavg)"
for i in 1 2 3; do
    OSOE_TEST_FLOOR_PINS=1 ctest --test-dir build/release -R '^shmem\.floor$' -V > "$OUT/floor-pins-$i.txt" 2>&1
    say "floor pins, run $i: $(grep -E 'tests passed' "$OUT/floor-pins-$i.txt")"
    grep -E 'pins|reported:' "$OUT/floor-pins-$i.txt" | sed 's/^[0-9]*: //' | head -8 | tee -a "$SUMMARY"
done
cmake -S . -B build/tier3 -DOSOE_TEST_PINS=ON > "$OUT/configure-tier3-pins.txt" 2>&1 &&
    cmake --build build/tier3 -j4 > "$OUT/build-tier3-pins.txt" 2>&1 || say "tier3 with the pins did not configure or build"
ctest --test-dir build/tier3 -L tier3 -R 'floor' -V --timeout 600 > "$OUT/tier3-floor-pins.txt" 2>&1
say "tier3's floor tests with the pins: $(grep -E 'tests passed' "$OUT/tier3-floor-pins.txt")"
grep -E '\(Failed\)|\(Timeout\)' "$OUT/tier3-floor-pins.txt" | head -8 | tee -a "$SUMMARY"
say "== $(date -u +%T) done"
