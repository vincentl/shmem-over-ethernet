#!/bin/bash
# SPDX-License-Identifier: LGPL-3.0-or-later
# v0.2 step 4 part 2: the circulant hang as the suite met it - the 8x2 and
# 5.perturb cases started beside the 64-node one, ROUNDS times; a run still
# going at 150 s (the small ones) is dumped (osoe-stat of each node).
# usage: repro-mix.sh TAR OUTDIR ROUNDS. Takes the VM lock.
exec 9>/tmp/osoe-vm.lock; flock -w 10800 9 || exit 1
TAR=$1; D=$2; ROUNDS=$3; mkdir -p $D; OUT=$D/repro-mix.txt; : > $OUT
T=/tmp/part2-repro-mix; rm -rf $T; mkdir -p $T; tar -C $T -xf $TAR; cd $T
cmake --preset debug > /dev/null 2>&1
if ! cmake --build build/debug -j4 > $D/build.txt 2>&1; then echo "BUILD FAILED" >> $OUT; exit 1; fi
B=$T/build/debug
LINK="OSOE_LINK_HOP_NS=86000 OSOE_LINK_MESSAGE_NS=7000 OSOE_LINK_BYTES_PER_SECOND=116000000 SHMEM_SYMMETRIC_SIZE=12m"
big=$(seq -s, 0 63)
for round in $(seq 1 $ROUNDS); do
  echo "== round $round $(date +%H:%M:%S)" >> $OUT
  env $LINK OSOE_JOB_ID=$((round * 100 + 1)) $B/tools/launch/osoe-launch -n 64 -m $big -t 500 -- \
      $B/shmem/tests/shmem_test_circulant-reduce > $D/run-$round-64.txt 2>&1 &
  p64=$!
  sleep 20
  env $LINK OSOE_JOB_ID=$((round * 100 + 2)) $B/tools/launch/osoe-launch -n 16 \
      -m 0,0,1,1,2,2,3,3,4,4,5,5,6,6,7,7 -t 300 -- $B/shmem/tests/shmem_test_circulant-reduce \
      > $D/run-$round-8x2.txt 2>&1 &
  p8=$!
  env $LINK OSOE_JOB_ID=$((round * 100 + 3)) OSOE_TEST_PERTURB=1 $B/tools/launch/osoe-launch -n 5 \
      -m 0,1,2,3,4 -t 300 -- $B/shmem/tests/shmem_test_circulant-reduce > $D/run-$round-5p.txt 2>&1 &
  p5=$!
  for s in $(seq 1 150); do sleep 1; kill -0 $p8 2>/dev/null || kill -0 $p5 2>/dev/null || break; done
  if kill -0 $p8 2>/dev/null || kill -0 $p5 2>/dev/null; then
    echo "a small run hung at 150 s; dumping" >> $OUT
    for seg in $(ls /dev/shm | grep -E '^osoe\.(1|2|3)0[23]\.[0-9]+\.[0-9]+\.0$|^osoe\.[0-9]+0[23]\.0\.[0-9]+\.0$'); do
      $B/tools/stat/osoe-stat /$seg > $D/stat-$round-$seg.txt 2>&1
    done
    ls /dev/shm > $D/shm-$round.txt
  fi
  wait $p8; echo "8x2 exit $?" >> $OUT
  wait $p5; echo "5.perturb exit $?" >> $OUT
  wait $p64; echo "64 exit $?" >> $OUT
  grep -h -m1 -E "CIRCULANT|osoe_fatal|deadlock" $D/run-$round-*.txt >> $OUT
done
cd /; rm -rf $T
echo "DONE $(date +%H:%M:%S)" >> $OUT
