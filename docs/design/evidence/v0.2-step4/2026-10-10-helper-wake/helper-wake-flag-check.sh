#!/bin/bash
# SPDX-License-Identifier: LGPL-3.0-or-later
# shmem.helper-wake waiting for the owner's sleeping flag before each put (the
# release checklist's item; the merge point's verification): main's test and
# the branch's, each run 20 times alone and 20 times beside four busy loops -
# where main's failed 9 runs of 20 on 2026-10-10 - and three mutants of the
# branch, each run five times, every run of which must fail: a helper that
# spins 200 us after raising its flag (the scale review's mutate.py), the same
# at 30 us, and a wake sent to the node's other helper.
# usage: helper-wake-flag-check.sh MAIN_TAR BRANCH_TAR OUTDIR. Takes the VM lock.
exec 9>/tmp/osoe-vm.lock; flock -w 43200 9 || exit 1
MAIN=$1; BRANCH=$2; D=$3; mkdir -p "$D"; OUT=$D/check.txt; : > "$OUT"
MUTATE=<repo>/build/scratch/review-v0.2-step4/scale/mutant/mutate.py
T=/tmp/helper-wake-flag-check; rm -rf "${T:?}"
for t in main branch spin200 spin30 misroute; do mkdir -p "$T/$t"; done
tar -C "$T/main" -xf "$MAIN"
for t in branch spin200 spin30 misroute; do tar -C "$T/$t" -xf "$BRANCH"; done
python3 "$MUTATE" "$T/spin200" > /dev/null
python3 "$MUTATE" "$T/spin30" > /dev/null
python3 - "$T/spin30/shmem/src/progress-host.c" <<'PYEOF'
import sys
p = sys.argv[1]; s = open(p).read()
a = "uint64_t spin_until = osoe_now_ns() + 200000;"
assert s.count(a) == 1
open(p, "w").write(s.replace(a, "uint64_t spin_until = osoe_now_ns() + 30000;"))
PYEOF
python3 - "$T/misroute/shmem/src/progress-host.c" <<'PYEOF'
import sys
p = sys.argv[1]; s = open(p).read()
a = "    deferred |= owner_bit(owner_of_peer(peer), osoe_am.area.helpers);\n    osoe_progress_host_wake_deferred();"
assert s.count(a) == 1
b = ("    uint32_t helpers = osoe_am.area.helpers;   /* MUTANT: the other helper */\n"
     "    deferred |= owner_bit(helpers ? owner_of_peer(peer) % helpers + 1 : 0, helpers);\n"
     "    osoe_progress_host_wake_deferred();")
open(p, "w").write(s.replace(a, b))
PYEOF
for t in main branch spin200 spin30 misroute; do
  ( cd "$T/$t" && cmake --preset release > /dev/null 2>&1 &&
    cmake --build build/release -j4 --target osoe osoe-launch shmem_test_helper-wake > "$D/build-$t.txt" 2>&1 ) \
    || { echo "build $t failed" >> "$OUT"; exit 1; }
  echo "build $t: warnings $(grep -c 'warning:' "$D/build-$t.txt")" >> "$OUT"
done
runs() {  # TREE COUNT LABEL
  local t=$1 n=$2 label=$3 pass=0
  for i in $(seq 1 "$n"); do
    if ctest --test-dir "$T/$t/build/release" -R '^shmem\.helper-wake\.' -V \
         > "$D/$label-$i.full" 2>&1; then pass=$((pass + 1)); fi
  done
  echo "$label: $pass of $n passed" >> "$OUT"
  # Each line "node N's wire, helper H's[, attempt A]: helper O woken W times": the
  # owner's counts (O = H) and the other helper's, as count x times.
  cat "$D/$label"-*.full |
    sed -n "s/.*wire, helper \([0-9]\)'s[^:]*: helper \([0-9]\) woken \([0-9]*\) times.*/\1 \2 \3/p" \
    > "$D/$label-woken.txt"
  echo "  the owner woken: $(awk '$1 == $2 { print $3 }' "$D/$label-woken.txt" | sort -n | uniq -c |
    awk '{ printf "%sx%s ", $1, $2 }')" >> "$OUT"
  echo "  the other helper woken: $(awk '$1 != $2 { print $3 }' "$D/$label-woken.txt" | sort -n |
    uniq -c | awk '{ printf "%sx%s ", $1, $2 }')" >> "$OUT"
  echo "  flags not up: $(cat "$D/$label"-*.full | grep -c 'flag not up')" >> "$OUT"
}
spin() { for i in 1 2 3 4; do ( exec 9>&-; while :; do :; done ) & SPINNERS="$SPINNERS $!"; done; }
unspin() { for q in $SPINNERS; do kill "$q" 2>/dev/null; done; wait 2>/dev/null; SPINNERS=""; }
echo "== quiet $(date +%H:%M:%S)" >> "$OUT"
runs main 20 main-quiet
runs branch 20 branch-quiet
echo "== beside four busy loops $(date +%H:%M:%S)" >> "$OUT"
SPINNERS=""; spin
runs main 20 main-loaded
runs branch 20 branch-loaded
unspin
echo "== mutants, each run must fail $(date +%H:%M:%S)" >> "$OUT"
for t in spin200 spin30 misroute; do runs "$t" 5 "mutant-$t"; done
cd /; rm -rf "${T:?}"
echo "DONE $(date +%H:%M:%S)" >> "$OUT"
