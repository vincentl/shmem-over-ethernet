#!/bin/bash
# SPDX-License-Identifier: LGPL-3.0-or-later
# collect's node totals by a direct exchange among the leads (P33 of
# docs/prompts/v0.2-open-items.md; docs/design/collect-scan-proposal.md, "A
# direct exchange of the totals"), two mutants beside their control: the
# branch's tree built as release, a copy of it whose exchange keeps one set of
# slots rather than two by the exchange's parity - exchange_model.py's
# one_slot - and a copy whose two sets share one counter - one_counter - each
# running shmem.collect-places.*, the zeros cases under OSOE_TEST_PERTURB as
# registered, ROUNDS times over. The control passes every case every time. Each
# mutant fails only cases the exchange takes, where a collect of nothing has no
# gather to order a lead's next write after another's read: one_slot nearly
# every one every time, each case's collects including one of nothing; and
# one_counter the zeros case every time, its collects of nothing back to back,
# and now and then another.
# usage: check.sh TAR OUTDIR [ROUNDS]. Takes the VM lock.
exec 9>/tmp/osoe-vm.lock; flock -w 21600 9 || exit 1
TAR=$1; D=$2; ROUNDS=${3:-3}; mkdir -p "$D"; OUT=$D/check.txt; : > "$OUT"
T=/tmp/collect-exchange-check; rm -rf "${T:?}"; mkdir -p "$T/control" "$T/one-slot" "$T/one-counter"
for t in control one-slot one-counter; do tar -C "$T/$t" -xf "$TAR"; done
python3 - "$T/one-slot/shmem/src/internode-generic.c" "$T/one-counter/shmem/src/internode-generic.c" <<'PYEOF'
import sys
slot, counter = sys.argv[1], sys.argv[2]
a = "    uint32_t parity = (uint32_t)(team_phase->exchanges % 2);\n"
s = open(slot).read(); assert s.count(a) == 1
open(slot, "w").write(s.replace(a, a +
    "    uint32_t counter = parity;\n"
    "    parity = 0;   /* MUTATION: one set of slots, the counters still two */\n"))
s = open(counter).read(); assert s.count(a) == 1
open(counter, "w").write(s.replace(a, a +
    "    uint32_t counter = 0;   /* MUTATION: one counter for both sets */\n"))
for p in (slot, counter):
    s = open(p).read()
    for old, new in (
        ("exchange_counter(number, parity),\n                           OSOE_RMW_ADD",
         "exchange_counter(number, counter),\n                           OSOE_RMW_ADD"),
        ("    team_phase->exchange_base[parity] += n - 1;\n",
         "    team_phase->exchange_base[counter] += n - 1;\n"),
        ("exchange_counter(number, parity), 8, false,\n"
         "                     OSOE_COMPARISON_GREATER_OR_EQUAL, team_phase->exchange_base[parity],",
         "exchange_counter(number, counter), 8, false,\n"
         "                     OSOE_COMPARISON_GREATER_OR_EQUAL, team_phase->exchange_base[counter],")):
        assert s.count(old) == 1, (p, old)
        s = s.replace(old, new)
    open(p, "w").write(s)
PYEOF
for t in one-slot one-counter; do
  echo "mutated lines $t: $(grep -c MUTATION "$T/$t/shmem/src/internode-generic.c")" >> "$OUT"
done
for t in control one-slot one-counter; do
  ( cd "$T/$t" && cmake --preset release > /dev/null 2>&1 && cmake --build build/release -j4 > "$T/build-$t.txt" 2>&1 ) \
    || { echo "build $t failed" >> "$OUT"; exit 1; }
  echo "build $t: warnings $(grep -c 'warning:' "$T/build-$t.txt")" >> "$OUT"
done
for t in control one-slot one-counter; do
  for r in $(seq 1 "$ROUNDS"); do
    echo "== $t round $r $(date +%H:%M:%S)" >> "$OUT"
    ctest --test-dir "$T/$t/build/release" -j4 --output-on-failure -R '^shmem\.collect-places\.' \
      > "$D/$t-$r.full" 2>&1
    grep -E "tests passed" "$D/$t-$r.full" >> "$OUT"
    grep -E "^\s+[0-9]+ - shmem" "$D/$t-$r.full" >> "$OUT"
    grep -m 2 -E "^osoe: PE [0-9]+: shmem_|byte [0-9]+ is|spans .* nodes, .*took way" "$D/$t-$r.full" >> "$OUT"
  done
done
cd /; rm -rf "${T:?}"
echo "DONE $(date +%H:%M:%S)" >> "$OUT"
