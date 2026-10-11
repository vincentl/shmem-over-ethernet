#!/bin/bash
# SPDX-License-Identifier: LGPL-3.0-or-later
# shmem.helper-wake's run at no helpers, where the lead's agent is the owner
# (scale S1's agent half): the branch, run 20 times alone and 20 beside four
# busy loops; and the scale review's agent mutant - the agent spinning up to
# 200 us before its idle wait (mutate-agent.py) - and the same at 30 us, each
# run five times, every run of which must fail.
# usage: helper-wake-agent-check.sh BRANCH_TAR OUTDIR. Takes the VM lock.
exec 9>/tmp/osoe-vm.lock; flock -w 43200 9 || exit 1
BRANCH=$1; D=$2; mkdir -p "$D"; OUT=$D/check.txt; : > "$OUT"
MUTATE=<repo>/build/scratch/review-v0.2-step4/scale/mutant/mutate-agent.py
T=/tmp/helper-wake-agent-check; rm -rf "${T:?}"
for t in branch spin200 spin30; do mkdir -p "$T/$t"; tar -C "$T/$t" -xf "$BRANCH"; done
python3 "$MUTATE" "$T/spin200" > /dev/null
python3 "$MUTATE" "$T/spin30" > /dev/null
python3 - "$T/spin30/shmem/src/progress-host.c" <<'PYEOF'
import sys
p = sys.argv[1]; s = open(p).read()
a = "uint64_t spin_until = osoe_now_ns() + 200000;"
assert s.count(a) == 1
open(p, "w").write(s.replace(a, "uint64_t spin_until = osoe_now_ns() + 30000;"))
PYEOF
for t in branch spin200 spin30; do
  ( cd "$T/$t" && cmake --preset release > /dev/null 2>&1 &&
    cmake --build build/release -j4 --target osoe osoe-launch shmem_test_helper-wake > "$D/build-$t.txt" 2>&1 ) \
    || { echo "build $t failed" >> "$OUT"; exit 1; }
  echo "build $t: warnings $(grep -c 'warning:' "$D/build-$t.txt")" >> "$OUT"
done
runs() {  # TREE COUNT LABEL REGEX
  local t=$1 n=$2 label=$3 regex=$4 pass=0
  for i in $(seq 1 "$n"); do
    if ctest --test-dir "$T/$t/build/release" -R "$regex" -V > "$D/$label-$i.full" 2>&1; then
      pass=$((pass + 1)); fi
  done
  echo "$label: $pass of $n passed" >> "$OUT"
  cat "$D/$label"-*.full |
    sed -n "s/.*wire, owner \([0-9]\)'s: owner \([0-9]\) woken \([0-9]*\) times.*/\3/p" | sort -n | uniq -c |
    awk '{ printf "%sx%s ", $1, $2 }' > "$D/$label-woken.txt"
  echo "  the owner woken: $(cat "$D/$label-woken.txt")" >> "$OUT"
  echo "  flags not up: $(cat "$D/$label"-*.full | grep -c 'flag not up')" >> "$OUT"
}
spin() { for i in 1 2 3 4; do ( exec 9>&-; while :; do :; done ) & SPINNERS="$SPINNERS $!"; done; }
unspin() { for q in $SPINNERS; do kill "$q" 2>/dev/null; done; wait 2>/dev/null; SPINNERS=""; }
AGENT='^shmem\.helper-wake\.agent\.'
BOTH='^shmem\.helper-wake\.'
echo "== quiet $(date +%H:%M:%S)" >> "$OUT"
runs branch 20 agent-quiet "$AGENT"
echo "== beside four busy loops $(date +%H:%M:%S)" >> "$OUT"
SPINNERS=""; spin
runs branch 20 both-loaded "$BOTH"
unspin
echo "== the agent mutants, each run must fail $(date +%H:%M:%S)" >> "$OUT"
for t in spin200 spin30; do runs "$t" 5 "mutant-$t" "$AGENT"; done
cd /; rm -rf "${T:?}"
echo "DONE $(date +%H:%M:%S)" >> "$OUT"
