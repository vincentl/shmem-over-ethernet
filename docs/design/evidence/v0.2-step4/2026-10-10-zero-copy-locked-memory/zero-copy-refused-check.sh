#!/bin/bash
# SPDX-License-Identifier: LGPL-3.0-or-later
# The zero-copy send refused for want of locked memory, as reworked after its
# read (docs/decisions/2026-10-10-zero-copy-locked-memory.md): the branch and
# three mutants built as release - one that goes on lending after a refusal,
# which the refused tests must fail; one that offers the refused bytes again
# only at a later pass, as first built, timed beside the branch; one without the
# fallback, which must fail them too. The static checks; the wire's and the
# internode tests on the branch; each mutant against the refused tests; the
# refused internode test five times each on the branch and the later-pass
# mutant, with their times; the probe as it is and under `ulimit -l 0`.
# usage: zero-copy-refused-check.sh TAR OUTDIR. Takes the VM lock.
exec 9>/tmp/osoe-vm.lock; flock -w 43200 9 || exit 1
TAR=$1; D=$2; mkdir -p "$D"; OUT=$D/check.txt; : > "$OUT"
T=/tmp/zero-copy-refused-check; rm -rf "${T:?}"
for t in branch keeps-lending later-pass no-fallback; do mkdir -p "$T/$t"; tar -C "$T/$t" -xf "$TAR"; done
mutate() {  # TREE OLD NEW: one occurrence of OLD in ring.c becomes NEW
  python3 - "$T/$1/shmem/src/ring.c" "$2" "$3" <<'PYEOF'
import sys
p, old, new = sys.argv[1:4]; s = open(p).read()
assert s.count(old) == 1, old
open(p, "w").write(s.replace(old, new))
PYEOF
}
mutate keeps-lending "                w->zero_copy = false;
                w->sending = false;" "                w->sending = false;"
mutate later-pass "                w->sending = false;
                w->sent = -1;" "                w->sent = 0;"
mutate no-fallback "            if ((cqe->res == -ENOMEM || cqe->res == -ENOBUFS) &&
                region < OSOE_WIRE_LENT_REGIONS) {" "            if (false) {"
for t in branch keeps-lending later-pass no-fallback; do
  ( cd "$T/$t" && cmake --preset release > /dev/null 2>&1 && cmake --build build/release -j4 > "$D/build-$t.txt" 2>&1 ) \
    || { echo "build $t failed" >> "$OUT"; grep -m5 'error' "$D/build-$t.txt" >> "$OUT"; exit 1; }
  echo "build $t: warnings $(grep -c 'warning:' "$D/build-$t.txt")" >> "$OUT"
done
B=$T/branch/build/release
echo "== static checks $(date +%H:%M:%S)" >> "$OUT"
ctest --test-dir "$B" -j4 --output-on-failure \
  -R '^shmem\.(memory-orders|strict-c11|boundary|static-link|exports|settings-documented|license-lines|version\.)' \
  > "$D/static.full" 2>&1
grep -E "tests passed|\(Failed\)" "$D/static.full" >> "$OUT"
echo "== the branch: the wire's and the internode tests $(date +%H:%M:%S)" >> "$OUT"
ctest --test-dir "$B" -j4 --output-on-failure -R '^shmem\.(wire|internode)' > "$D/branch.full" 2>&1
grep -E "tests passed|\(Failed\)|\(Timeout\)|Skipped" "$D/branch.full" >> "$OUT"
ctest --test-dir "$B" -V -R '^shmem\.(wire\.zero-copy-refused|internode\.zero-copy-refused)' > "$D/branch-refused.full" 2>&1
grep -E "zero copy: |refused for want|SKIPPED|Passed|Failed" "$D/branch-refused.full" | sed 's/^[0-9]*: //' | sort | uniq -c >> "$OUT"
for t in keeps-lending later-pass no-fallback; do
  echo "== mutant $t $(date +%H:%M:%S)" >> "$OUT"
  ctest --test-dir "$T/$t/build/release" -V --timeout 120 -R 'zero-copy-refused' > "$D/mutant-$t.full" 2>&1
  grep -E "tests passed|Test +#.*(Passed|Failed|Timeout)|zero copy: |Cannot allocate|timeout after" \
    "$D/mutant-$t.full" | sed 's/^[0-9]*: //' >> "$OUT"
done
echo "== the refused internode test, five times each, timed $(date +%H:%M:%S)" >> "$OUT"
for i in 1 2 3 4 5; do
  for t in branch later-pass; do
    ctest --test-dir "$T/$t/build/release" -R '^shmem\.internode\.zero-copy-refused' > "$D/time-$t-$i.full" 2>&1
    echo "$t $i: $(grep -oE 'Passed +[0-9.]+ sec|Failed.*' "$D/time-$t-$i.full" | head -1)" >> "$OUT"
  done
done
echo "== the probe $(date +%H:%M:%S)" >> "$OUT"
"$B/tools/probe/probe" > "$D/probe.txt" 2>&1; echo "probe: $?" >> "$OUT"
grep -E "locked-memory|PROBE" "$D/probe.txt" >> "$OUT"
( ulimit -l 0 && "$B/tools/probe/probe" ) > "$D/probe-limit-0.txt" 2>&1; echo "probe under ulimit -l 0: $?" >> "$OUT"
grep -E "locked-memory|FAIL|PROBE" "$D/probe-limit-0.txt" >> "$OUT"
cd /; rm -rf "${T:?}"
echo "DONE $(date +%H:%M:%S)" >> "$OUT"
