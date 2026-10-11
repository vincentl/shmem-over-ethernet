#!/bin/bash
# SPDX-License-Identifier: LGPL-3.0-or-later
# The control segment's size on main and on the branch blocking-gets, at 4, 64
# and 128 PEs on one node, by the probe's reading of its own mapping
# (segment-size.c) - for the README's memory rows: each thread place's context
# block, 128 bytes, is the branch's only change to it.
# usage: segment-size-check.sh MAIN_TAR BRANCH_TAR OUTDIR. Takes the VM lock.
exec 9>/tmp/osoe-vm.lock; flock -w 43200 9 || exit 1
MAIN=$1; BRANCH=$2; D=$3; mkdir -p "$D"; OUT=$D/check.txt; : > "$OUT"
T=/tmp/segment-size-check; rm -rf "${T:?}"; mkdir -p "$T/main" "$T/branch"
tar -C "$T/main" -xf "$MAIN"; tar -C "$T/branch" -xf "$BRANCH"
for t in main branch; do
  ( cd "$T/$t" && cmake --preset release > /dev/null 2>&1 &&
    cmake --build build/release -j4 > "$D/build-$t.txt" 2>&1 ) \
    || { echo "build $t failed" >> "$OUT"; exit 1; }
  "$T/$t/build/release/tools/wrapper/oshcc" -O2 -o "$T/$t/segment-size" "$(dirname "$0")/segment-size.c" \
    || { echo "oshcc $t failed" >> "$OUT"; exit 1; }
  for p in 4 64 128; do
    m=$(printf '0%.0s,' $(seq 1 $p)); m=${m%,}
    SHMEM_SYMMETRIC_SIZE=1m OSOE_SHM_CAPACITY_CHECK=0 "$T/$t/build/release/tools/launch/osoe-launch" \
      -n $p -m "$m" -t 300 -- "$T/$t/segment-size" > "$D/segment-$t-$p.txt" 2>&1
    echo "$t P=$p: $(grep '^segment bytes' "$D/segment-$t-$p.txt" | awk '{print $3}')" >> "$OUT"
  done
done
cd /; rm -rf "${T:?}"
echo "DONE $(date +%H:%M:%S)" >> "$OUT"
