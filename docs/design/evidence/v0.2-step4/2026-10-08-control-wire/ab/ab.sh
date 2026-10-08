#!/bin/bash
# v0.2 step 4 part 2: does the control wire slow the many-node tests, or did
# the host's other VM slow smoke-2? Part 1 (TAR_A) against Part 2 (TAR_B) on
# one VM, the host otherwise quiet: each built in debug, then ctest's own
# commands for the cases smoke-2 slowed, one at a time in the order A B B A,
# and then the suite's circulant group four at a time, A then B.
# usage: ab.sh TAR_A TAR_B OUTDIR. Takes the VM lock.
exec 9>/tmp/osoe-vm.lock; flock -w 10800 9 || exit 1
TAR_A=$1; TAR_B=$2; D=$3; mkdir -p "$D"; OUT=$D/ab.txt; : > "$OUT"
rm -rf /tmp/part2-ab-a /tmp/part2-ab-b
mkdir -p /tmp/part2-ab-a /tmp/part2-ab-b
tar -C /tmp/part2-ab-a -xf "$TAR_A"
tar -C /tmp/part2-ab-b -xf "$TAR_B"
for side in a b; do
  if ! (cd /tmp/part2-ab-$side && cmake --preset debug > /dev/null 2>&1 &&
        cmake --build build/debug -j4 > "$D/build-$side.txt" 2>&1); then
    echo "BUILD FAILED $side" >> "$OUT"; exit 1
  fi
done
ONE='^shmem\.circulant\.(17|33|64)$|^shmem\.shape\.chosen\.pi\.64$|^shmem\.shape\.back-to-back\.0-1-2-3-4-5-6-7-8-9-10-11-12-13-14-15$'
GROUP='^shmem\.circulant\.(33|64|70|8x2|5\.perturb)$'
run() { # side label regex jobs
  echo "== $2 $1 $(date +%H:%M:%S)" >> "$OUT"
  (cd /tmp/part2-ab-$1 && ctest --test-dir build/debug -R "$3" -j"$4" > "$D/$2-$1.txt" 2>&1)
  grep -E "Test +#" "$D/$2-$1.txt" | sed -E 's/^ *[0-9]+\/[0-9]+ //' >> "$OUT"
}
run a one-1 "$ONE" 1
run b one-1 "$ONE" 1
run b one-2 "$ONE" 1
run a one-2 "$ONE" 1
run a group "$GROUP" 4
run b group "$GROUP" 4
rm -rf /tmp/part2-ab-a /tmp/part2-ab-b
echo "DONE $(date +%H:%M:%S)" >> "$OUT"
