#!/bin/bash
# SPDX-License-Identifier: LGPL-3.0-or-later
# SOS's unit/spam at 4 x 2 failed at OSOE_PROGRESS_HELPERS=2 in the merge
# point's sweep (exit 1, no osoe diagnostic, its last line its fcollect's) and
# passed at none. Three runs at none and three at two, timed, the launcher given
# 300 s; a run still going at 150 s gets osoe-stat's summary and dump.
# usage: spam-repro.sh TAR OUTDIR. Takes the VM lock.
exec 9>/tmp/osoe-vm.lock; flock -w 21600 9 || exit 1
TAR=$1; D=$2; mkdir -p "$D"; OUT=$D/spam.txt; : > "$OUT"
T=/tmp/spam-repro; rm -rf "${T:?}"; mkdir -p "$T"; tar -C "$T" -xf "$TAR"; cd "$T" || exit 1
cmake --preset release > /dev/null 2>&1 && cmake --build build/release -j4 > /dev/null 2>&1 || { echo "build failed" >> "$OUT"; exit 1; }
SOS=<repo>/corpus/tests-sos/test
build/release/tools/wrapper/oshcc -O1 -g -I$SOS/include $SOS/unit/spam.c -o spam -lm -pthread 2> "$D/spam.build" || { echo "spam build failed" >> "$OUT"; exit 1; }
L=build/release/tools/launch/osoe-launch; S=build/release/tools/stat/osoe-stat
for h in 0 2 0 2 0 2; do
  i=$((i + 1)); start=$(date +%s.%N)
  OSOE_PROGRESS_HELPERS=$h $L -n 8 -m 0,0,1,1,2,2,3,3 -t 300 -- ./spam > "$D/spam-$i-h$h.out" 2>&1 &
  pid=$!; dumped=no
  for s in $(seq 1 300); do
    kill -0 $pid 2>/dev/null || break
    if [ $s -eq 150 ]; then
      $S --summary $pid > "$D/spam-$i-h$h-summary.txt" 2>&1; $S -d $pid > "$D/spam-$i-h$h-dump.txt" 2>&1; dumped=yes
    fi
    sleep 1
  done
  wait $pid; rc=$?
  took=$(echo "$start $(date +%s.%N)" | awk '{printf "%.1f", $2 - $1}')
  echo "run $i h=$h rc=$rc took=${took}s dumped=$dumped last: $(grep -v '^osoe' "$D/spam-$i-h$h.out" | grep -v '^\s*$' | tail -1 | cut -c1-90)" >> "$OUT"
done
cd /; rm -rf "${T:?}"
echo "DONE $(date +%H:%M:%S)" >> "$OUT"
