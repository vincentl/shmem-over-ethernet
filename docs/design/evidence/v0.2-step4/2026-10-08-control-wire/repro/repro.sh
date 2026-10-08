#!/bin/bash
# v0.2 step 4 part 2: reproduce shmem.circulant.5.perturb's hang with the
# deadlock detector on, and dump every node's segment while it hangs.
# usage: repro.sh TAR OUTDIR CASE (CASE: 5.perturb or 8x2). Takes the VM lock.
exec 9>/tmp/osoe-vm.lock; flock -w 10800 9 || exit 1
TAR=$1; D=$2; C=$3; mkdir -p $D; OUT=$D/repro-$C.txt; : > $OUT
T=/tmp/part2-repro; rm -rf $T; mkdir -p $T; tar -C $T -xf $TAR; cd $T
cmake --preset debug > /dev/null 2>&1
if ! cmake --build build/debug -j4 > $D/build.txt 2>&1; then echo "BUILD FAILED" >> $OUT; exit 1; fi
B=$T/build/debug
if [ $C = 8x2 ]; then n=16; map=0,0,1,1,2,2,3,3,4,4,5,5,6,6,7,7; extra=""; else n=5; map=0,1,2,3,4; extra="OSOE_TEST_PERTURB=1"; fi
for attempt in 1 2 3 4 5 6 7 8 9 10; do
  echo "== attempt $attempt $(date +%H:%M:%S)" >> $OUT
  env OSOE_LINK_HOP_NS=86000 OSOE_LINK_MESSAGE_NS=7000 OSOE_LINK_BYTES_PER_SECOND=116000000 \
      SHMEM_SYMMETRIC_SIZE=12m OSOE_DEADLOCK_SECONDS=10 $extra \
      $B/tools/launch/osoe-launch -n $n -m $map -t 90 -- $B/shmem/tests/shmem_test_circulant-reduce \
      > $D/run-$C-$attempt.txt 2>&1 &
  pid=$!
  for s in $(seq 1 45); do sleep 1; kill -0 $pid 2>/dev/null || break; done
  if kill -0 $pid 2>/dev/null; then
    echo "hung at 45 s; dumping" >> $OUT
    for seg in $(ls /dev/shm | grep -E '^osoe\.[0-9]+\.[0-9]+\.[0-9]+\.0$'); do
      $B/tools/stat/osoe-stat /$seg > $D/stat-$C-$attempt-$seg.txt 2>&1
    done
  fi
  wait $pid; echo "exit $?" >> $OUT
  grep -m5 -E "CIRCULANT|deadlock|osoe_fatal|osoe:" $D/run-$C-$attempt.txt >> $OUT
done
cd /; rm -rf $T
echo "DONE $(date +%H:%M:%S)" >> $OUT
