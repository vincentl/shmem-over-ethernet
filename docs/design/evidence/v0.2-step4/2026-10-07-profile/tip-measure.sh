#!/bin/bash
# SPDX-License-Identifier: LGPL-3.0-or-later
# v0.2 step 4 part 0: the tip (fill + the unsent queue capped + 256 KiB) against
# the first rung alone (fill2), over loopback: the 64 KiB put stream, the
# mailbox's rate, and shmem.floor's stream ratio for the tip and its poll
# mutant (the STREAM pin). Builds in /tmp/ladder. Takes the VM lock.
exec 9>/tmp/osoe-vm.lock; flock -w 10800 9 || exit 1
OUT=$1; mkdir -p $OUT; : > $OUT/tip.txt
S=<repo>/build/scratch/ladder
for b in fill2 tip; do
  W=/tmp/ladder/$b/work; mkdir -p $W
  [ -x $W/putstream ] || /tmp/ladder/$b/build/release/tools/wrapper/oshcc -O2 -o $W/putstream $S/putstream.c
done
echo "== the 64 KiB put stream" >> $OUT/tip.txt
for i in 1 2 3; do for b in fill2 tip; do
  B=/tmp/ladder/$b/build/release
  r=$($B/tools/launch/osoe-launch -n 2 -m 0,1 -t 120 -- /tmp/ladder/$b/work/putstream 65536 10 2>&1 | grep '^putstream')
  echo "round=$i build=$b $r" | tee -a $OUT/tip.txt
done; done
echo "== the mailbox, 4 PEs on 2 nodes, 20 s" >> $OUT/tip.txt
for i in 1 2; do for b in fill2 tip; do
  B=/tmp/ladder/$b/build/release
  r=$($B/tools/launch/osoe-launch -n 4 -m 0,0,1,1 -t 120 -- $B/tools/pacer/osoe-pacer --seconds 20 mailbox 2>&1 | grep '^summary')
  echo "round=$i build=$b $r" | tee -a $OUT/tip.txt
done; done
echo "== shmem.floor's stream ratio: the tip and its poll mutant" >> $OUT/tip.txt
for i in 1 2 3; do for b in tip poll-tip; do
  B=/tmp/ladder/$b/build/release
  r=$($B/tools/launch/osoe-launch -n 2 -m 0,1 -t 300 -- $B/shmem/tests/shmem_test_floor 2>&1 | grep -oE 'stream_mib_per_s floor=[0-9]+ library=[0-9]+|stream library/floor=[0-9.]+ \([0-9.-]+\)' | tail -2 | paste -sd' ' -)
  echo "round=$i build=$b $r" | tee -a $OUT/tip.txt
done; done
echo DONE >> $OUT/tip.txt
