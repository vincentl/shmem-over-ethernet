#!/bin/bash
# v0.2 step 4 part 2: barrier-many.c at 16, 33 and 64 logical nodes, Part 1
# (TAR_A) against Part 2 (TAR_B), each built in release, alternating A B A B.
# usage: probe.sh TAR_A TAR_B OUTDIR [COUNT]. Takes the VM lock.
exec 9>/tmp/osoe-vm.lock; flock -w 10800 9 || exit 1
TAR_A=$1; TAR_B=$2; D=$3; COUNT=${4:-1000}; mkdir -p "$D"; OUT=$D/probe.txt; : > "$OUT"
HERE=$(dirname "$0")
rm -rf /tmp/part2-probe-a /tmp/part2-probe-b
mkdir -p /tmp/part2-probe-a /tmp/part2-probe-b
tar -C /tmp/part2-probe-a -xf "$TAR_A"
tar -C /tmp/part2-probe-b -xf "$TAR_B"
for side in a b; do
  if ! (cd /tmp/part2-probe-$side && cmake --preset release > /dev/null 2>&1 &&
        cmake --build build/release -j4 > "$D/build-$side.txt" 2>&1 &&
        build/release/tools/wrapper/oshcc -O2 -o barrier-many "$HERE/barrier-many.c"); then
    echo "BUILD FAILED $side" >> "$OUT"; exit 1
  fi
done
LINK="OSOE_LINK_HOP_NS=86000 OSOE_LINK_MESSAGE_NS=7000 OSOE_LINK_BYTES_PER_SECOND=116000000"
for round in 1 2; do
  for side in a b; do
    for n in 16 33 64; do
      T=/tmp/part2-probe-$side
      map=$(seq -s, 0 $((n - 1)))
      line=$(env $LINK $T/build/release/tools/launch/osoe-launch -n $n -m $map -t 300 -- \
             $T/barrier-many "$COUNT" 2>&1 | grep -E "barrier-many|osoe_fatal|error" | head -3)
      echo "$side round=$round $line" >> "$OUT"
    done
  done
done
rm -rf /tmp/part2-probe-a /tmp/part2-probe-b
echo "DONE $(date +%H:%M:%S)" >> "$OUT"
