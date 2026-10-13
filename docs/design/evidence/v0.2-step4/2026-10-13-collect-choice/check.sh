#!/bin/bash
# SPDX-License-Identifier: LGPL-3.0-or-later
# collect's places chosen among four ways (P31 of
# docs/prompts/v0.2-open-items.md; design §5.5, "The choice among the four"),
# two mutants beside their control: the branch's tree built as release; a copy
# whose default takes each node's total wherever the team allows it, as before
# P31 - "as before"; and a copy whose exchange costs no hop - "no hop" - each
# running shmem.collect-places.* and shmem.shape.*. The control passes every
# case. "as before" fails the cases whose link the test sets by hand, the
# Pi's, where the default must take the exchange; "no hop" fails the places'
# grid, where the C must give the model's answers and costs, line for line.
# usage: check.sh TAR OUTDIR. Takes the VM lock.
exec 9>/tmp/osoe-vm.lock; flock -w 21600 9 || exit 1
TAR=$1; D=$2; mkdir -p "$D"; OUT=$D/check.txt; : > "$OUT"
T=/tmp/collect-choice-check; rm -rf "${T:?}"; mkdir -p "$T/control" "$T/as-before" "$T/no-hop"
for t in control as-before no-hop; do tar -C "$T/$t" -xf "$TAR"; done
python3 - "$T/as-before/shmem/src/collectives.c" "$T/no-hop/shmem/src/collective-shape.c" <<'PYEOF'
import sys
before, hop = sys.argv[1], sys.argv[2]
s = open(before).read()
a = "    if (setting == OSOE_COLLECTIVE_PLACES_LINK)\n        return places_chosen("
assert s.count(a) == 1
open(before, "w").write(s.replace(a,
    "    if (setting == OSOE_COLLECTIVE_PLACES_LINK)   /* MUTATION: the default as before */\n"
    "        return OSOE_COLLECT_WAY_TOTALS;\n"
    "    if (0)\n        return places_chosen("))
s = open(hop).read()
a = "        return add_saturating(link->hop_ns,\n                              multiply_saturating(nodes - 1,"
assert s.count(a) == 1
open(hop, "w").write(s.replace(a,
    "        return add_saturating(0 /* MUTATION: no hop */,\n"
    "                              multiply_saturating(nodes - 1,"))
PYEOF
for t in as-before no-hop; do
  f=$( [ "$t" = as-before ] && echo collectives.c || echo collective-shape.c )
  echo "mutated lines $t: $(grep -c MUTATION "$T/$t/shmem/src/$f")" >> "$OUT"
done
for t in control as-before no-hop; do
  ( cd "$T/$t" && cmake --preset release > /dev/null 2>&1 && cmake --build build/release -j4 > "$T/build-$t.txt" 2>&1 ) \
    || { echo "build $t failed" >> "$OUT"; exit 1; }
  echo "build $t: warnings $(grep -c 'warning:' "$T/build-$t.txt")" >> "$OUT"
done
for t in control as-before no-hop; do
  echo "== $t $(date +%H:%M:%S)" >> "$OUT"
  ctest --test-dir "$T/$t/build/release" -j4 --output-on-failure \
    -R '^shmem\.(collect-places\.|shape\.)' > "$D/$t.full" 2>&1
  grep -E "tests passed" "$D/$t.full" >> "$OUT"
  grep -E "^\s+[0-9]+ - shmem" "$D/$t.full" >> "$OUT"
  grep -m 2 -E "took way|where the C and the model differ" "$D/$t.full" >> "$OUT"
done
cd /; rm -rf "${T:?}"
echo "DONE $(date +%H:%M:%S)" >> "$OUT"
