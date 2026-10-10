#!/bin/bash
# SPDX-License-Identifier: LGPL-3.0-or-later
# Keepalive's tip, after its verification at e2bb1e9. What changed since: the
# silent-peer check's SHMEM_DEBUG line, the keepalive test reading both leads'
# TCP sockets, tier3.pi.keepalive.default-on.0-1, comments and documents. So:
# the static checks; the four presets and tier3 built, warnings counted; the
# debug suite at no helpers, where every SHMEM_DEBUG test runs; the keepalive
# tests in every preset, verbose, so their lines are kept; tier 3's keepalive,
# vanished-node and wire-pair tests; four mutants, each of which a keepalive
# test must fail; and correctness Q2-1's reproduction - circulant 33, 64 and 70
# at once, no helpers, ten busy loops - with OSOE_KEEPALIVE=20,5,6 and then
# unset, the kernel's counters read around each.
# usage: verify-keepalive-final.sh TAR OUTDIR. Takes the VM lock.
exec 9>/tmp/osoe-vm.lock; flock -w 43200 9 || exit 1
TAR=$1; D=$2; mkdir -p "$D"; OUT=$D/verify.txt; : > "$OUT"
T=/tmp/v02-keepalive-final; rm -rf "${T:?}"; mkdir -p "$T"; tar -C "$T" -xf "$TAR"; cd "$T" || exit 1
KEEP='^shmem\.(keepalive\.|wire-pair$|stopped-node\.keepalive$|stopped-bulk|settings-documented$)'
lines() { grep -E "connected TCP sockets|silent-peer check|probes its connections" "$1" |
          sed 's/^[0-9]*: //' | sort | uniq -c >> "$OUT"; }

echo "== static checks $(date +%H:%M:%S)" >> "$OUT"
cmake --preset release > /dev/null 2>&1 && cmake --build build/release -j4 > "$D/build-release.txt" 2>&1 \
  || { echo "BUILD FAILED release" >> "$OUT"; exit 1; }
echo "release warnings: $(grep -c 'warning:' "$D/build-release.txt")" >> "$OUT"
ctest --test-dir build/release -j4 --output-on-failure \
  -R '^shmem\.(memory-orders|strict-c11|boundary|static-link|exports|settings-documented|license-lines|version\.)' \
  > "$D/static.full" 2>&1
grep -E "tests passed|\(Failed\)" "$D/static.full" >> "$OUT"
echo "== release, the keepalive tests $(date +%H:%M:%S)" >> "$OUT"
ctest --test-dir build/release -j4 -V -R "$KEEP" > "$D/release-keepalive.full" 2>&1
grep -E "tests passed|\(Failed\)|\(Timeout\)" "$D/release-keepalive.full" >> "$OUT"
lines "$D/release-keepalive.full"
rm -rf build/release

echo "== debug $(date +%H:%M:%S)" >> "$OUT"
cmake --preset debug > /dev/null 2>&1 && cmake --build build/debug -j4 > "$D/build-debug.txt" 2>&1 \
  || { echo "BUILD FAILED debug" >> "$OUT"; exit 1; }
echo "warnings: $(grep -c 'warning:' "$D/build-debug.txt")" >> "$OUT"
OSOE_PROGRESS_HELPERS=0 ctest --test-dir build/debug -j4 --output-on-failure > "$D/debug-0.full" 2>&1
grep -E "tests passed|\(Failed\)|\(Timeout\)" "$D/debug-0.full" >> "$OUT"
ctest --test-dir build/debug -j4 -V -R "$KEEP" > "$D/debug-keepalive.full" 2>&1
grep -E "tests passed|\(Failed\)|\(Timeout\)" "$D/debug-keepalive.full" >> "$OUT"
lines "$D/debug-keepalive.full"

echo "== Q2-1's reproduction $(date +%H:%M:%S)" >> "$OUT"
counters() {
  local d=0; while read -r a b rest; do d=$((d + 16#$b)); done < /proc/net/softnet_stat
  printf "softnet-dropped=%d " $d
  awk 'NR==1{for(i=1;i<=NF;i++) n[i]=$i} NR==2{for(i=1;i<=NF;i++) if(n[i]=="TCPAbortOnTimeout"||n[i]=="TCPKeepAlive"||n[i]=="TCPTimeouts") printf "%s=%s ", n[i], $i}' \
    <(grep "^TcpExt:" /proc/net/netstat)
}
reproduce() {  # TAG, then env's arguments
  local tag=$1; shift
  echo "-- $tag ($*): before $(counters)" >> "$OUT"
  local pids="" q
  for i in $(seq 1 10); do ( exec 9>&-; while :; do :; done ) & pids="$pids $!"; done
  local start=$(date +%s)
  env "$@" OSOE_PROGRESS_HELPERS=0 timeout 900 ctest --test-dir build/debug -j4 --timeout 600 \
    --output-on-failure -R '^shmem\.circulant\.(33|64|70)$' > "$D/q2-1-$tag.full" 2>&1 9>&-
  local rc=$?
  for q in $pids; do kill "$q" 2>/dev/null; done; wait 2>/dev/null
  echo "-- $tag: after $(counters)" >> "$OUT"
  echo "-- $tag: rc=$rc, $(( $(date +%s) - start )) s, lines saying a node stopped answering: $(grep -c 'stopped answering' "$D/q2-1-$tag.full")" >> "$OUT"
  grep -E "tests passed|\(Failed\)|\(Timeout\)|Test +#.*(Passed|Failed)" "$D/q2-1-$tag.full" >> "$OUT"
}
reproduce set OSOE_KEEPALIVE=20,5,6
reproduce default -u OSOE_KEEPALIVE
rm -rf build/debug

for p in asan tsan; do
  echo "== $p $(date +%H:%M:%S)" >> "$OUT"
  cmake --preset $p > /dev/null 2>&1 && cmake --build build/$p -j4 > "$D/build-$p.txt" 2>&1 \
    || { echo "BUILD FAILED $p" >> "$OUT"; continue; }
  echo "warnings: $(grep -c 'warning:' "$D/build-$p.txt")" >> "$OUT"
  ctest --test-dir build/$p -j4 -V -R "$KEEP" > "$D/$p-keepalive.full" 2>&1
  grep -E "tests passed|\(Failed\)|\(Timeout\)" "$D/$p-keepalive.full" >> "$OUT"
  lines "$D/$p-keepalive.full"
  rm -rf build/$p
done

echo "== tier3 $(date +%H:%M:%S)" >> "$OUT"
cmake --preset tier3 > /dev/null 2>&1 && cmake --build build/tier3 -j4 > "$D/build-tier3.txt" 2>&1 \
  || { echo "BUILD FAILED tier3" >> "$OUT"; exit 1; }
echo "warnings: $(grep -c 'warning:' "$D/build-tier3.txt")" >> "$OUT"
ctest --test-dir build/tier3 -V --timeout 600 -R '^tier3\.pi\.(keepalive\.|vanished-node|wire-pair$)' \
  > "$D/tier3.full" 2>&1
grep -E "tests passed|\(Failed\)|\(Timeout\)" "$D/tier3.full" >> "$OUT"
lines "$D/tier3.full"
rm -rf build/tier3

# Each mutant built in the tier3 preset, which registers both kinds of test.
mutant() {  # NAME OLD NEW: one occurrence of OLD in progress-host.c becomes NEW
  local name=$1 M=/tmp/v02-keepalive-mutant
  echo "== mutant $name $(date +%H:%M:%S)" >> "$OUT"
  rm -rf "${M:?}"; mkdir -p "$M"; tar -C "$M" -xf "$TAR"
  python3 - "$M/shmem/src/progress-host.c" "$2" "$3" <<'PYEOF'
import sys
p, old, new = sys.argv[1:4]; s = open(p).read()
assert s.count(old) == 1, old
open(p, "w").write(s.replace(old, new))
PYEOF
  [ $? -eq 0 ] || { echo "mutation $name did not apply" >> "$OUT"; return; }
  ( cd "$M" && cmake --preset tier3 > /dev/null 2>&1 &&
    cmake --build build/tier3 -j4 --target osoe osoe-launch shmem_test_keepalive > "$D/build-mutant-$name.txt" 2>&1 ) \
    || { echo "mutant $name: build failed" >> "$OUT"; return; }
  ctest --test-dir "$M/build/tier3" -V -R '^(shmem|tier3\.pi)\.keepalive\.(loopback-off|set-on|default-on)' \
    > "$D/mutant-$name.full" 2>&1
  grep -E "tests passed|Test +#.*(Passed|Failed)" "$D/mutant-$name.full" >> "$OUT"
  lines "$D/mutant-$name.full"
  rm -rf "${M:?}"
}
mutant off-everywhere \
  "    if (every_node_loopback && !osoe.bootstrap.keepalive_set)" \
  "    if (!osoe.bootstrap.keepalive_set)"
mutant no-default \
  "    if (every_node_loopback && !osoe.bootstrap.keepalive_set)
        osoe.bootstrap.keepalive_off = true;" \
  "    (void)every_node_loopback;"
mutant check-on-over-loopback \
  "    if (b->keepalive_off) {
        silent_budget_ms = 0;" \
  "    if (b->keepalive_off && b->keepalive_set) {
        silent_budget_ms = 0;"
mutant check-off-between-machines \
  "    if (b->keepalive_off) {
        silent_budget_ms = 0;" \
  "    if (b->keepalive_off || !b->keepalive_set) {
        silent_budget_ms = 0;"
cd /; rm -rf "${T:?}"
echo "DONE $(date +%H:%M:%S)" >> "$OUT"
