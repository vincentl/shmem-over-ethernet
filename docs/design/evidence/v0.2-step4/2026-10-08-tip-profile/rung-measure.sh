#!/bin/bash
# v0.2 step 4 part 0: a rung against the build below it, over loopback, alternating:
# the 64 KiB and 1 MiB put streams, shmem.floor's stream ratio and the mailbox.
# usage: rung-measure.sh OUT BASE RUNG (builds in /tmp/ladder). Takes the VM lock.
exec 9>/tmp/osoe-vm.lock; flock -w 10800 9 || exit 1
OUT=$1; BASE=$2; RUNG=$3; mkdir -p $OUT; : > $OUT/measure.txt
S=/Users/vincent/shmem-over-ethernet/build/scratch/ladder
for b in $BASE $RUNG; do
  W=/tmp/ladder/$b/work; mkdir -p $W
  [ -x $W/putstream ] || /tmp/ladder/$b/build/release/tools/wrapper/oshcc -O2 -g -o $W/putstream $S/putstream.c || exit 1
done
for bytes in 65536 1048576; do
  echo "== the put stream, $bytes-byte puts, 10 s" >> $OUT/measure.txt
  for i in 1 2 3; do for b in $BASE $RUNG; do
    B=/tmp/ladder/$b/build/release
    r=$($B/tools/launch/osoe-launch -n 2 -m 0,1 -t 120 -- /tmp/ladder/$b/work/putstream $bytes 10 2>&1 | grep '^putstream')
    echo "round=$i build=$b $r" | tee -a $OUT/measure.txt
  done; done
done
echo "== shmem.floor's stream ratio" >> $OUT/measure.txt
for i in 1 2 3; do for b in $BASE $RUNG; do
  B=/tmp/ladder/$b/build/release
  r=$($B/tools/launch/osoe-launch -n 2 -m 0,1 -t 300 -- $B/shmem/tests/shmem_test_floor 2>&1 | grep -oE 'stream_mib_per_s floor=[0-9]+ library=[0-9]+|stream library/floor=[0-9.]+ \([0-9.-]+\)' | tail -2 | paste -sd' ' -)
  echo "round=$i build=$b $r" | tee -a $OUT/measure.txt
done; done
echo "== the mailbox, 4 PEs on 2 nodes, 20 s" >> $OUT/measure.txt
for i in 1 2; do for b in $BASE $RUNG; do
  B=/tmp/ladder/$b/build/release
  r=$($B/tools/launch/osoe-launch -n 4 -m 0,0,1,1 -t 120 -- $B/tools/pacer/osoe-pacer --seconds 20 mailbox 2>&1 | grep '^summary')
  echo "round=$i build=$b $r" | tee -a $OUT/measure.txt
done; done
echo DONE >> $OUT/measure.txt
