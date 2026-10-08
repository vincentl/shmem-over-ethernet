#!/bin/bash
# v0.2 step 4 part 2: the circulant hang under load - COPIES copies of the 8x2
# case at once (16 PEs each on 4 CPUs), ROUNDS times; a copy still running at
# 60 s is dumped (osoe-stat of each node, and the processes' stacks).
# usage: repro-load.sh TAR OUTDIR COPIES ROUNDS. Takes the VM lock.
exec 9>/tmp/osoe-vm.lock; flock -w 10800 9 || exit 1
TAR=$1; D=$2; COPIES=$3; ROUNDS=$4; mkdir -p $D; OUT=$D/repro-load.txt; : > $OUT
T=/tmp/part2-repro-load; rm -rf $T; mkdir -p $T; tar -C $T -xf $TAR; cd $T
cmake --preset debug > /dev/null 2>&1
if ! cmake --build build/debug -j4 > $D/build.txt 2>&1; then echo "BUILD FAILED" >> $OUT; exit 1; fi
B=$T/build/debug
map=0,0,1,1,2,2,3,3,4,4,5,5,6,6,7,7
for round in $(seq 1 $ROUNDS); do
  echo "== round $round $(date +%H:%M:%S)" >> $OUT
  pids=""
  for c in $(seq 1 $COPIES); do
    env OSOE_LINK_HOP_NS=86000 OSOE_LINK_MESSAGE_NS=7000 OSOE_LINK_BYTES_PER_SECOND=116000000 \
        SHMEM_SYMMETRIC_SIZE=12m OSOE_JOB_ID=$((round * 100 + c)) \
        $B/tools/launch/osoe-launch -n 16 -m $map -t 150 -- $B/shmem/tests/shmem_test_circulant-reduce \
        > $D/run-$round-$c.txt 2>&1 &
    pids="$pids $!"
  done
  for s in $(seq 1 60); do sleep 1; alive=0; for p in $pids; do kill -0 $p 2>/dev/null && alive=1; done; [ $alive = 0 ] && break; done
  if [ $alive = 1 ]; then
    echo "a copy hung at 60 s; dumping" >> $OUT
    for seg in $(ls /dev/shm | grep -E '^osoe\.[0-9]+\.[0-9]+\.[0-9]+\.0$'); do
      $B/tools/stat/osoe-stat /$seg > $D/stat-$round-$seg.txt 2>&1
    done
    for p in $(pgrep -f shmem_test_circulant-reduce); do
      echo "--- pid $p" >> $D/stacks-$round.txt
      gdb -p $p -batch -ex "thread apply all bt 12" >> $D/stacks-$round.txt 2>&1 || true
    done
  fi
  for p in $pids; do wait $p; echo "copy exit $?" >> $OUT; done
  grep -h -m1 -E "CIRCULANT|osoe_fatal|deadlock" $D/run-$round-*.txt >> $OUT
done
cd /; rm -rf $T
echo "DONE $(date +%H:%M:%S)" >> $OUT
