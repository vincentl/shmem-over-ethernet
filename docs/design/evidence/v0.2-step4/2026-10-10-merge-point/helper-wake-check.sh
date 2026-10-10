#!/bin/bash
# SPDX-License-Identifier: LGPL-3.0-or-later
# shmem.helper-wake run again a short count (v0.2 step 4's merge point): the
# test alone, 20 times each in release and under AddressSanitizer, 20 times
# under AddressSanitizer beside four busy loops; then the scale review's
# mutant, a helper spinning before it sleeps, at 200 and 30 microseconds, 10
# times each - every one of which must still fail.
# usage: helper-wake-check.sh TAR MUTATE_PY OUTDIR. Takes the VM lock.
exec 9>/tmp/osoe-vm.lock; flock -w 21600 9 || exit 1
TAR=$1; MUTATE=$2; D=$3; mkdir -p "$D"; OUT=$D/helper-wake-check.txt; : > "$OUT"
T=/tmp/helper-wake-check; rm -rf "${T:?}"; mkdir -p "$T/base"; tar -C "$T/base" -xf "$TAR"
runs() {   # label, build directory, count
  pass=0; fail=0
  for i in $(seq 1 "$3"); do
    if ctest --test-dir "$2" -R '^shmem\.helper-wake\.0-1-2-3$' --output-on-failure > "$D/$1-$i.txt" 2>&1 &&
       grep -q "100% tests passed" "$D/$1-$i.txt"; then pass=$((pass + 1)); else fail=$((fail + 1)); fi
  done
  echo "$1: $pass passed, $fail failed of $3; attempts past the first: $(cat "$D/$1"-*.txt | grep -c 'attempt [23]: helper')" >> "$OUT"
}
cd "$T/base" || exit 1
for p in release asan; do
  cmake --preset $p > /dev/null 2>&1 && cmake --build build/$p -j4 > "$D/build-$p.txt" 2>&1 || { echo "build $p failed" >> "$OUT"; exit 1; }
done
runs release build/release 20
runs asan build/asan 20
pids=""
for i in 1 2 3 4; do ( while :; do :; done ) & pids="$pids $!"; done
runs asan-loaded build/asan 20
for pid in $pids; do kill "$pid"; done
wait 2>/dev/null
for spin in 200000 30000; do
  M=$T/mutant-$spin; mkdir -p "$M"; tar -C "$M" -xf "$TAR"
  python3 "$MUTATE" "$M" > /dev/null && sed -i "s/osoe_now_ns() + 200000/osoe_now_ns() + $spin/" "$M/shmem/src/progress-host.c"
  grep -c "osoe_now_ns() + $spin" "$M/shmem/src/progress-host.c" >> "$OUT"
  ( cd "$M" && cmake --preset release > /dev/null 2>&1 && cmake --build build/release -j4 > "$D/build-mutant-$spin.txt" 2>&1 ) || { echo "mutant $spin build failed" >> "$OUT"; continue; }
  runs "mutant-spin-$spin-ns" "$M/build/release" 10
done
cd /; rm -rf "${T:?}"
echo "DONE $(date +%H:%M:%S)" >> "$OUT"
