#!/bin/bash
# SPDX-License-Identifier: LGPL-3.0-or-later
# Round six's U1 in the osoe VM, where the review measured 32,768 bytes at 1.17
# to 2.39 times 32,736: piece-edge with main before the fix (BEFORE_TAR) and
# with it (AFTER_TAR), release builds, four logical nodes of one PE, alternating,
# three runs a side. Usage: vm-u1.sh BEFORE_TAR AFTER_TAR OUT. Takes the VM lock.
exec 9>/tmp/osoe-vm.lock; flock -w 3600 9 || exit 1
B=$1 A=$2 OUT=$3
mkdir -p $OUT; : > $OUT/run.txt
P=<repo>/build/scratch/review-e/pi/piece-edge.c
for side in before after; do
  T=/tmp/u1-$side; rm -rf $T; mkdir -p $T
  if [ $side = before ]; then tar -C $T -xf $B; else tar -C $T -xf $A; fi
  (cd $T && cmake --preset release > /dev/null 2>&1 && cmake --build build/release -j4 > /dev/null 2>&1) || { echo "$side build failed" >> $OUT/run.txt; exit 1; }
  $T/build/release/tools/wrapper/oshcc -O2 -o $T/edge $P || exit 1
  echo "$side built" >> $OUT/run.txt
done
for i in 1 2 3; do
  for side in before after; do
    T=/tmp/u1-$side
    $T/build/release/tools/launch/osoe-launch -n 4 -m 0,1,2,3 -t 300 -- $T/edge > $OUT/vm-$side-$i.txt 2>&1
    echo "$side $i: $?" >> $OUT/run.txt
  done
done
rm -rf /tmp/u1-before /tmp/u1-after
echo "DONE $(date +%H:%M:%S)" >> $OUT/run.txt
