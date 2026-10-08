#!/bin/bash
# v0.2 step 4 part 2: after-barriers.c at 64 logical nodes, Part 1 (TAR_A)
# against Part 2 (TAR_B), each built in release, alternating A B three times.
# usage: after.sh TAR_A TAR_B OUTDIR [COUNT] [NODES]. Takes the VM lock.
exec 9>/tmp/osoe-vm.lock; flock -w 10800 9 || exit 1
TAR_A=$1; TAR_B=$2; D=$3; COUNT=${4:-1000}; N=${5:-64}; mkdir -p "$D"; OUT=$D/after.txt; : > "$OUT"
HERE=$(dirname "$0")
rm -rf /tmp/part2-after-a /tmp/part2-after-b
mkdir -p /tmp/part2-after-a /tmp/part2-after-b
tar -C /tmp/part2-after-a -xf "$TAR_A"
tar -C /tmp/part2-after-b -xf "$TAR_B"
for side in a b; do
  if ! (cd /tmp/part2-after-$side && cmake --preset release > /dev/null 2>&1 &&
        cmake --build build/release -j4 > "$D/build-$side.txt" 2>&1 &&
        build/release/tools/wrapper/oshcc -O2 -o after-barriers "$HERE/after-barriers.c"); then
    echo "BUILD FAILED $side" >> "$OUT"; exit 1
  fi
done
LINK="OSOE_LINK_HOP_NS=86000 OSOE_LINK_MESSAGE_NS=7000 OSOE_LINK_BYTES_PER_SECOND=116000000"
map=$(seq -s, 0 $((N - 1)))
for round in 1 2 3; do
  for side in a b; do
    T=/tmp/part2-after-$side
    line=$( ( TIMEFORMAT="real=%R user=%U sys=%S"
              time env $LINK $T/build/release/tools/launch/osoe-launch -n $N -m $map -t 300 -- \
                $T/after-barriers "$COUNT" 2>&1 | grep -E "after-barriers|osoe_fatal" | head -2 ) 2>&1 )
    echo "$side round=$round $(echo $line)" >> "$OUT"
  done
done
rm -rf /tmp/part2-after-a /tmp/part2-after-b
echo "DONE $(date +%H:%M:%S)" >> "$OUT"
