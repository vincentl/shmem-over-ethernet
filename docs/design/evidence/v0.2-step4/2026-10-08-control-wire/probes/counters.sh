#!/bin/bash
# v0.2 step 4 part 2: where Part 2's small reductions lose time at 64 logical
# nodes - the whole job's CPU time (bash's time over the launcher,
# which waits for its PEs), and every node's engine counters twice, 2 s apart,
# a few seconds into a loop of 8-byte sum reductions (osoe-stat -d -z), Part 1
# (TAR_A) against Part 2 (TAR_B), alternating A B A B.
# usage: counters.sh TAR_A TAR_B OUTDIR [COUNT]. Takes the VM lock.
exec 9>/tmp/osoe-vm.lock; flock -w 10800 9 || exit 1
TAR_A=$1; TAR_B=$2; D=$3; COUNT=${4:-20000}; mkdir -p "$D"; OUT=$D/counters.txt; : > "$OUT"
HERE=$(dirname "$0")
rm -rf /tmp/part2-counters-a /tmp/part2-counters-b
mkdir -p /tmp/part2-counters-a /tmp/part2-counters-b
tar -C /tmp/part2-counters-a -xf "$TAR_A"
tar -C /tmp/part2-counters-b -xf "$TAR_B"
for side in a b; do
  if ! (cd /tmp/part2-counters-$side && cmake --preset release > /dev/null 2>&1 &&
        cmake --build build/release -j4 > "$D/build-$side.txt" 2>&1 &&
        build/release/tools/wrapper/oshcc -O2 -o reduce-loop "$HERE/reduce-loop.c"); then
    echo "BUILD FAILED $side" >> "$OUT"; exit 1
  fi
done
LINK="OSOE_LINK_HOP_NS=86000 OSOE_LINK_MESSAGE_NS=7000 OSOE_LINK_BYTES_PER_SECOND=116000000"
map=$(seq -s, 0 63)
job=7100
for round in 1 2; do
  for side in a b; do
    T=/tmp/part2-counters-$side
    job=$((job + 1))
    echo "== $side round=$round job=$job $(date +%H:%M:%S)" >> "$OUT"
    ( TIMEFORMAT="real=%R user=%U sys=%S"
      time env $LINK OSOE_JOB_ID=$job $T/build/release/tools/launch/osoe-launch -n 64 -m $map \
        -t 300 -- $T/reduce-loop "$COUNT" > "$D/run-$side-$round.txt" 2>&1 ) \
      2> "$D/time-$side-$round.txt" &
    pid=$!
    sleep 5
    $T/build/release/tools/stat/osoe-stat -d -z $job > "$D/stat-$side-$round-1.txt" 2>&1
    sleep 2
    $T/build/release/tools/stat/osoe-stat -d -z $job > "$D/stat-$side-$round-2.txt" 2>&1
    wait $pid
    echo "exit $?" >> "$OUT"
    grep -h "reduce-loop\|osoe_fatal" "$D/run-$side-$round.txt" >> "$OUT"
    cat "$D/time-$side-$round.txt" >> "$OUT"
  done
done
rm -rf /tmp/part2-counters-a /tmp/part2-counters-b
echo "DONE $(date +%H:%M:%S)" >> "$OUT"
