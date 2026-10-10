#!/bin/bash
# SPDX-License-Identifier: LGPL-3.0-or-later
# Keepalive off by default on one machine's loopback (the author, 2026-10-10,
# item 1): the release build, the two new tests and the tests that set
# keepalive themselves; then a mutant without the default - the lead leaving
# keepalive as it was - which must fail shmem.keepalive.loopback-off.
# usage: keepalive-check.sh TAR OUTDIR. Takes the VM lock.
exec 9>/tmp/osoe-vm.lock; flock -w 21600 9 || exit 1
TAR=$1; D=$2; mkdir -p "$D"; OUT=$D/keepalive-check.txt; : > "$OUT"
T=/tmp/keepalive-check; rm -rf "${T:?}"; mkdir -p "$T/base" "$T/mutant"
tar -C "$T/base" -xf "$TAR"; tar -C "$T/mutant" -xf "$TAR"
python3 - "$T/mutant/shmem/src/progress-host.c" <<'PYEOF'
import sys
p = sys.argv[1]; s = open(p).read()
a = "    if (one_machine && !osoe.bootstrap.keepalive_set)\n        osoe.bootstrap.keepalive_off = true;"
assert s.count(a) == 1
open(p, "w").write(s.replace(a, "    (void)one_machine;"))
PYEOF
for t in base mutant; do
  ( cd "$T/$t" && cmake --preset release > /dev/null 2>&1 && cmake --build build/release -j4 > "$D/build-$t.txt" 2>&1 ) \
    || { echo "build $t failed" >> "$OUT"; exit 1; }
done
echo "== base" >> "$OUT"
ctest --test-dir "$T/base/build/release" -j4 --output-on-failure \
  -R '^shmem\.(keepalive\.|wire-pair$|stopped-node\.keepalive|stopped-bulk|helper-wake|refuse\.keepalive)' \
  > "$D/base.full" 2>&1
grep -E "tests passed|Test +#.*(Passed|Failed)|KEEPALIVE|connected sockets" "$D/base.full" >> "$OUT"
echo "== mutant" >> "$OUT"
ctest --test-dir "$T/mutant/build/release" --output-on-failure -R '^shmem\.keepalive\.' > "$D/mutant.full" 2>&1
grep -E "tests passed|Test +#.*(Passed|Failed)|connected sockets" "$D/mutant.full" >> "$OUT"
cd /; rm -rf "${T:?}"
echo "DONE $(date +%H:%M:%S)" >> "$OUT"
