#!/bin/bash
# SPDX-License-Identifier: LGPL-3.0-or-later
# collect's places by a scan of the nodes' totals (P30 of
# docs/prompts/v0.2-open-items.md; docs/design/collect-scan-proposal.md), a
# mutant beside its control: the branch's tree built as release, and a copy of
# it whose gather by ranges sends at its last step all a lead holds, uncut at
# its receiver's own share - the model's no_cut - each running
# shmem.collect-places.*. The control passes every case; the mutant fails every
# scan case with a team over a node count that is not a power of two - where
# the last step sends fewer shares than the lead holds - which the receiver's
# check of the record names, the writer's step running on into its own share:
# all but the two-node maps', whose teams span two nodes or one; and passes the
# scan on the map numbered out of the ranks' order, which falls back to each
# node's total, and every case the scan is not forced in.
# usage: check.sh TAR OUTDIR. Takes the VM lock.
exec 9>/tmp/osoe-vm.lock; flock -w 21600 9 || exit 1
TAR=$1; D=$2; mkdir -p "$D"; OUT=$D/check.txt; : > "$OUT"
T=/tmp/collect-scan-check; rm -rf "${T:?}"; mkdir -p "$T/control" "$T/mutant"
tar -C "$T/control" -xf "$TAR"; tar -C "$T/mutant" -xf "$TAR"
python3 - "$T/mutant/shmem/src/internode-generic.c" <<'PYEOF'
import sys
p = sys.argv[1]; s = open(p).read()
a = "        if (s + 1 == steps) {\n            /* The cut,"
assert s.count(a) == 1
open(p, "w").write(s.replace(a,
     "        if (0 && s + 1 == steps) {   /* MUTATION: the last step uncut */\n"
     "            /* The cut,"))
PYEOF
echo "mutated lines: $(grep -c MUTATION "$T/mutant/shmem/src/internode-generic.c")" >> "$OUT"
for t in control mutant; do
  ( cd "$T/$t" && cmake --preset release > /dev/null 2>&1 && cmake --build build/release -j4 > "$T/build-$t.txt" 2>&1 ) \
    || { echo "build $t failed" >> "$OUT"; exit 1; }
  echo "build $t: warnings $(grep -c 'warning:' "$T/build-$t.txt")" >> "$OUT"
done
for t in control mutant; do
  echo "== $t $(date +%H:%M:%S)" >> "$OUT"
  ctest --test-dir "$T/$t/build/release" -j4 --output-on-failure -R '^shmem\.collect-places\.' \
    > "$D/$t.full" 2>&1
  grep -E "tests passed|Test +#[0-9]+: " "$D/$t.full" | sed -e 's/^ *[0-9]*\/[0-9]* *Test *#[0-9]*: //' >> "$OUT"
  grep -m 2 -E "^osoe: PE [0-9]+: shmem_|byte [0-9]+ is" "$D/$t.full" >> "$OUT"
done
cd /; rm -rf "${T:?}"
echo "DONE $(date +%H:%M:%S)" >> "$OUT"
