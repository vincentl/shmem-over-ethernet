#!/bin/bash
# SPDX-License-Identifier: LGPL-3.0-or-later
# shmem.helper-wake after its adversarial read, as kept: one wake a round judged
# at 90 in 100, and PE 0's claim of the put's own wake at 90 in 100 puts (the
# two-wake form, helper-wake-second-check.sh, failed a correct library beside
# four busy loops). The branch's two runs (two helpers; the agent at none), 20
# times each alone and beside four busy loops in the release build, and 20
# times each beside four busy loops under AddressSanitizer - the merge point's
# own measure, where counting wakes without the flag failed 9 of 20 (finding
# 3); and six mutants in the release build, five runs each alone and five
# beside four busy loops, every run of which must fail: a helper spinning 200
# us after raising its flag, and 30 us (the scale review's mutate.py, copied
# into helper-wake-mutants/); the agent spinning 200 and 30 us at no helpers
# (mutate-agent.py); a put's wake sent to the node's other helper; and a put's
# wake held back for good, which the quiet's covers (finding 2).
# usage: helper-wake-final-check.sh TAR OUTDIR. Takes the VM lock.
exec 9>/tmp/osoe-vm.lock; flock -w 43200 9 || exit 1
TAR=$1; D=$2; mkdir -p "$D"; OUT=$D/check.txt; : > "$OUT"
MUTANTS=$(cd "$(dirname "$0")" && pwd)/helper-wake-mutants
T=/tmp/helper-wake-final-check; rm -rf "${T:?}"
TREES="branch spin200 spin30 agent200 agent30 misroute held"
for t in $TREES; do mkdir -p "$T/$t"; tar -C "$T/$t" -xf "$TAR"; done
python3 "$MUTANTS/mutate.py" "$T/spin200" > /dev/null
python3 "$MUTANTS/mutate.py" "$T/spin30" > /dev/null
python3 "$MUTANTS/mutate-agent.py" "$T/agent200" > /dev/null
python3 "$MUTANTS/mutate-agent.py" "$T/agent30" > /dev/null
for t in spin30 agent30; do
  python3 - "$T/$t/shmem/src/progress-host.c" <<'PYEOF'
import sys
p = sys.argv[1]; s = open(p).read()
a = "uint64_t spin_until = osoe_now_ns() + 200000;"
assert s.count(a) == 1
open(p, "w").write(s.replace(a, "uint64_t spin_until = osoe_now_ns() + 30000;"))
PYEOF
done
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
python3 - "$T/held/shmem/src/transport-internode.c" <<'PYEOF'
import sys
p = sys.argv[1]; s = open(p).read()
a = """    if (flags & OSOE_TRANSPORT_MORE) {
        osoe_count(OSOE_C_AM_WAKES_DEFERRED, 1);
        osoe_progress_host_defer(peer);
        return;
    }
    osoe_progress_host_wake(peer);
}"""
assert s.count(a) == 1
b = """    (void)flags;   /* MUTANT: every put's wake held back, for the next wake to cover */
    osoe_count(OSOE_C_AM_WAKES_DEFERRED, 1);
    osoe_progress_host_defer(peer);
}"""
open(p, "w").write(s.replace(a, b))
PYEOF
for t in $TREES; do
  ( cd "$T/$t" && cmake --preset release > /dev/null 2>&1 &&
    cmake --build build/release -j4 --target osoe osoe-launch shmem_test_helper-wake > "$D/build-$t.txt" 2>&1 ) \
    || { echo "build $t failed" >> "$OUT"; exit 1; }
  echo "build $t: warnings $(grep -c 'warning:' "$D/build-$t.txt")" >> "$OUT"
done
( cd "$T/branch" && cmake --preset asan > /dev/null 2>&1 &&
  cmake --build build/asan -j4 --target osoe osoe-launch shmem_test_helper-wake > "$D/build-asan.txt" 2>&1 ) \
  || { echo "build asan failed" >> "$OUT"; exit 1; }
runs() {  # TREE PRESET TEST COUNT LABEL
  local t=$1 p=$2 test=$3 n=$4 label=$5 pass=0
  for i in $(seq 1 "$n"); do
    if ctest --test-dir "$T/$t/build/$p" -R "^shmem\.helper-wake\.$test" -V \
         > "$D/$label-$i.full" 2>&1; then pass=$((pass + 1)); fi
  done
  echo "$label: $pass of $n passed" >> "$OUT"
  # Each line "node N's wire, owner H's: owner O woken W times in R rounds":
  # the owner's counts (O = H), and every other owner's, as count x times.
  cat "$D/$label"-*.full |
    sed -n "s/.*wire, owner \([0-9]\)'s: owner \([0-9]\) woken \([0-9]*\) times.*/\1 \2 \3/p" \
    > "$D/$label-woken.txt"
  echo "  lines read: $(wc -l < "$D/$label-woken.txt")" >> "$OUT"
  echo "  the owner woken: $(awk '$1 == $2 { print $3 }' "$D/$label-woken.txt" | sort -n | uniq -c |
    awk '{ printf "%sx%s ", $1, $2 }')" >> "$OUT"
  echo "  the other owners woken: $(awk '$1 != $2 { print $3 }' "$D/$label-woken.txt" | sort -n |
    uniq -c | awk '{ printf "%sx%s ", $1, $2 }')" >> "$OUT"
  echo "  flags not up: $(cat "$D/$label"-*.full | grep -c 'flag not up')" >> "$OUT"
  echo "  PE 0's claims at the puts: $(cat "$D/$label"-*.full |
    sed -n "s/.*PE 0 claimed a wake at \([0-9]*\) of.*/\1/p" | sort -n | uniq -c |
    awk '{ printf "%sx%s ", $1, $2 }')" >> "$OUT"
}
spin() { for i in 1 2 3 4; do ( exec 9>&-; while :; do :; done ) & SPINNERS="$SPINNERS $!"; done; }
unspin() { for q in $SPINNERS; do kill "$q" 2>/dev/null; done; wait 2>/dev/null; SPINNERS=""; }
SPINNERS=""
echo "== the branch, alone $(date +%H:%M:%S)" >> "$OUT"
runs branch release '0-1-2-3$' 20 helpers-quiet
runs branch release 'agent' 20 agent-quiet
echo "== the branch, beside four busy loops $(date +%H:%M:%S)" >> "$OUT"
spin
runs branch release '0-1-2-3$' 20 helpers-loaded
runs branch release 'agent' 20 agent-loaded
runs branch asan '0-1-2-3$' 20 asan-helpers-loaded
runs branch asan 'agent' 20 asan-agent-loaded
unspin
echo "== mutants, every run must fail $(date +%H:%M:%S)" >> "$OUT"
for t in spin200 spin30 misroute held; do runs "$t" release '0-1-2-3$' 5 "mutant-$t-quiet"; done
for t in agent200 agent30 held; do runs "$t" release 'agent' 5 "mutant-$t-agent-quiet"; done
spin
for t in spin200 spin30 misroute held; do runs "$t" release '0-1-2-3$' 5 "mutant-$t-loaded"; done
for t in agent200 agent30 held; do runs "$t" release 'agent' 5 "mutant-$t-agent-loaded"; done
unspin
cd /; rm -rf "${T:?}"
echo "DONE $(date +%H:%M:%S)" >> "$OUT"
