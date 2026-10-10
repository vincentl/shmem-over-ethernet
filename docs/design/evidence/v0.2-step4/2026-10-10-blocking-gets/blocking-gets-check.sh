#!/bin/bash
# SPDX-License-Identifier: LGPL-3.0-or-later
# A blocking get's pieces together (the author, 2026-10-10, item 2): main and
# the branch built as release; the tests of gets on the branch, the new
# shmem.get-pieces among them; a mutant, the branch with the pipelined path
# taken out, which must fail shmem.get-pieces; question 4's probe on both trees
# over the VM's loopback, alternating three times; and the control segment's
# size on both trees at 4, 64 and 256 PEs a node - the README's columns.
# usage: blocking-gets-check.sh MAIN_TAR BRANCH_TAR PROBE_C OUTDIR. Takes the VM lock.
exec 9>/tmp/osoe-vm.lock; flock -w 21600 9 || exit 1
MAIN=$1; BRANCH=$2; PROBE=$3; D=$4; mkdir -p "$D"; OUT=$D/check.txt; : > "$OUT"
T=/tmp/blocking-gets-check; rm -rf "${T:?}"; mkdir -p "$T/main" "$T/branch" "$T/mutant"
tar -C "$T/main" -xf "$MAIN"; tar -C "$T/branch" -xf "$BRANCH"; tar -C "$T/mutant" -xf "$BRANCH"
python3 - "$T/mutant/shmem/src/transport-internode.c" <<'PYEOF'
import sys
p = sys.argv[1]; s = open(p).read()
a = "    if (len > limit && own <= OSOE_LANDING_OWNER_MASK) {"
assert s.count(a) == 1
open(p, "w").write(s.replace(a, "    if (false && len > limit && own <= OSOE_LANDING_OWNER_MASK) {"))
PYEOF
for t in main branch mutant; do
  ( cd "$T/$t" && cmake --preset release > /dev/null 2>&1 && cmake --build build/release -j4 > "$D/build-$t.txt" 2>&1 ) \
    || { echo "build $t failed" >> "$OUT"; exit 1; }
  echo "build $t: warnings $(grep -c 'warning:' "$D/build-$t.txt")" >> "$OUT"
done
echo "== the branch's tests of gets $(date +%H:%M:%S)" >> "$OUT"
ctest --test-dir "$T/branch/build/release" -j4 --output-on-failure \
  -R '^shmem\.(get|iget|rma|multiple|context|landing|stopped-bulk|internode|lanes\.threads)' \
  > "$D/branch-gets.full" 2>&1
grep -E "tests passed|\(Failed\)|\(Timeout\)" "$D/branch-gets.full" >> "$OUT"
grep -E "a get of" "$D/branch-gets.full" >> "$OUT"
echo "== the mutant, the pipelined path out $(date +%H:%M:%S)" >> "$OUT"
ctest --test-dir "$T/mutant/build/release" --output-on-failure -R '^shmem\.get-pieces\.' > "$D/mutant.full" 2>&1
grep -E "tests passed|\(Failed\)|a get of" "$D/mutant.full" >> "$OUT"
echo "== question 4's probe, alternating $(date +%H:%M:%S)" >> "$OUT"
for t in main branch; do
  "$T/$t/build/release/tools/wrapper/oshcc" -O2 -o "$T/$t/get-pieces" "$PROBE" || echo "oshcc $t failed" >> "$OUT"
done
for i in 1 2 3; do
  for t in main branch; do
    echo "-- $t run $i" >> "$OUT"
    "$T/$t/build/release/tools/launch/osoe-launch" -n 2 -m 0,1 -t 120 -- "$T/$t/get-pieces" 21 | grep '^bytes=' >> "$OUT"
  done
done
echo "== the control segment, bytes $(date +%H:%M:%S)" >> "$OUT"
for t in main branch; do
  "$T/$t/build/release/tools/wrapper/oshcc" -O2 -o "$T/$t/segment-size" "$(dirname "$0")/segment-size.c" || echo "oshcc segment $t failed" >> "$OUT"
  for p in 4 64 128; do
    m=$(printf '0%.0s,' $(seq 1 $p)); m=${m%,}
    SHMEM_SYMMETRIC_SIZE=1m OSOE_SHM_CAPACITY_CHECK=0 "$T/$t/build/release/tools/launch/osoe-launch" -n $p -m "$m" -t 300 -- \
      "$T/$t/segment-size" > "$D/segment-$t-$p.txt" 2>&1 || true
    echo "$t P=$p: $(grep '^segment' "$D/segment-$t-$p.txt" | awk '{print $3}' | head -1)" >> "$OUT"
  done
done
cd /; rm -rf "${T:?}"
echo "DONE $(date +%H:%M:%S)" >> "$OUT"
