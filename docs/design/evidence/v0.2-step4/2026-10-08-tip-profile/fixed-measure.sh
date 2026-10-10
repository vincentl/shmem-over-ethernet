#!/bin/bash
# SPDX-License-Identifier: LGPL-3.0-or-later
# v0.2 step 4 part 0: the wires as registered files, by OSOE_EXPERIMENT_FIXED_FILES in
# one experiment build, alternating: the 64 KiB put stream, the mailbox, and
# osoe-bench's latency and atomic round trips. usage: fixed-measure.sh OUT NAME
exec 9>/tmp/osoe-vm.lock; flock -w 10800 9 || exit 1
OUT=$1; NAME=$2; mkdir -p $OUT; : > $OUT/fixed.txt
S=<repo>/build/scratch/ladder
B=/tmp/ladder/$NAME/build/release; W=/tmp/ladder/$NAME/work; mkdir -p $W
[ -x $W/putstream ] || $B/tools/wrapper/oshcc -O2 -g -o $W/putstream $S/putstream.c || exit 1
L=$B/tools/launch/osoe-launch
for i in 1 2 3; do for t in 0 1; do
  r=$(env OSOE_EXPERIMENT_FIXED_FILES=$t $L -n 2 -m 0,1 -t 120 -- $W/putstream 65536 10 2>&1 | grep '^putstream')
  echo "round=$i fixed=$t $r" | tee -a $OUT/fixed.txt
  r=$(env OSOE_EXPERIMENT_FIXED_FILES=$t $L -n 4 -m 0,0,1,1 -t 120 -- $B/tools/pacer/osoe-pacer --seconds 20 mailbox 2>&1 | grep '^summary')
  echo "round=$i fixed=$t $r" | tee -a $OUT/fixed.txt
  env OSOE_EXPERIMENT_FIXED_FILES=$t $L -n 2 -m 0,1 -t 300 -- $B/tools/bench/osoe-bench latency atomic > $OUT/bench-$i-$t.txt 2>&1
  echo "round=$i fixed=$t $(grep -E 'bytes=(8|65536|1048576) |^atomic' $OUT/bench-$i-$t.txt | grep -oE '(put\+quiet|get|fetch_add|atomic)[^ ]* .*bytes=[0-9]+|median_us=[0-9.]+' | paste -sd' ' -)" | tee -a $OUT/fixed.txt
done; done
echo DONE >> $OUT/fixed.txt
