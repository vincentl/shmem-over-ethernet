#!/bin/bash
# SPDX-License-Identifier: LGPL-3.0-or-later
# collect's places from each node's total (P29 of docs/prompts/v0.2-open-items.md),
# a mutant beside its control: the branch's tree built as release, and a copy of
# it whose bases are summed in the order of the node set's positions rather than
# of the team's ranks - the model's bases_in_node_order - each running
# shmem.collect-places.*. The control passes every case; the mutant fails the
# negative-stride ones, whose runs meet the nodes in the reverse of their order,
# and passes the block and cyclic ones, where the two orders are one or the
# places are the members' vector.
# usage: check.sh TAR OUTDIR. Takes the VM lock.
exec 9>/tmp/osoe-vm.lock; flock -w 21600 9 || exit 1
TAR=$1; D=$2; mkdir -p "$D"; OUT=$D/check.txt; : > "$OUT"
T=/tmp/collect-node-totals-check; rm -rf "${T:?}"; mkdir -p "$T/control" "$T/mutant"
tar -C "$T/control" -xf "$TAR"; tar -C "$T/mutant" -xf "$TAR"
python3 - "$T/mutant/shmem/src/collectives.c" <<'PYEOF'
import sys
p = sys.argv[1]; s = open(p).read()
a = ("    for (uint32_t j = 0; j < nodes; j++) {\n"
     "        uint64_t node_total = totals[runs[j].position];\n")
assert s.count(a) == 1
open(p, "w").write(s.replace(a,
     "    for (uint32_t p = 0, j = 0; p < nodes; p++) {\n"
     "        for (j = 0; runs[j].position != p; j++)\n"
     "            ;   /* MUTATION: the bases summed in position order */\n"
     "        uint64_t node_total = totals[runs[j].position];\n"))
PYEOF
echo "mutated lines: $(grep -c MUTATION "$T/mutant/shmem/src/collectives.c")" >> "$OUT"
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
  grep -m 2 -E "^osoe|byte [0-9]+ is" "$D/$t.full" >> "$OUT"
done
cd /; rm -rf "${T:?}"
echo "DONE $(date +%H:%M:%S)" >> "$OUT"
