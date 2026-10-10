#!/bin/bash
# SPDX-License-Identifier: LGPL-3.0-or-later
# A blocking get's pieces together, on the Pi (the author, 2026-10-10, item 2):
# main against the branch blocking-gets, question 4's probe (get-pieces.c)
# alternating three times between two nodes, 21 repetitions a size; then the
# branch's tests of gets on one node, its logical nodes over loopback. The
# branch in ~/osoe, main in ~/osoe/build/base-src, as pi-compare.sh does.
# usage: pi-blocking-gets.sh MAIN_TAR BRANCH_TAR PROBE_C OUTDIR (from the Mac).
set -u
MAIN=$1 BRANCH=$2 PROBE=$3 OUT=$4
mkdir -p "$OUT/trees/main" "$OUT/trees/branch"; LOG=$OUT/run.txt; : > "$LOG"
tar -C "$OUT/trees/main" -xf "$MAIN"; tar -C "$OUT/trees/branch" -xf "$BRANCH"
echo "start $(date +%H:%M:%S)" >> "$LOG"
rsync -a --delete --exclude=/build --exclude=/.git "$OUT/trees/branch"/ pi-login:osoe/ \
  || { echo "rsync failed" >> "$LOG"; exit 1; }
ssh pi-login 'rm -rf osoe/build/ladder osoe/build/base-src && mkdir -p osoe/build/ladder osoe/build/base-src' || exit 1
rsync -a --exclude=/build --exclude=/.git "$OUT/trees/main"/ pi-login:osoe/build/base-src/ || exit 1
scp -q "$PROBE" pi-login:osoe/build/ladder/get-pieces.c || exit 1
ssh pi-login 'cd osoe && cmake --preset release > /dev/null 2>&1 && cmake --build build/release -j4 > build/ladder/build-branch.txt 2>&1; echo "branch built: $?, warnings $(grep -c warning: build/ladder/build-branch.txt)"
  build/release/tools/wrapper/oshcc -O2 -o build/ladder/get-pieces-branch build/ladder/get-pieces.c; echo "probe branch: $?"
  cd build/base-src && cmake --preset release > /dev/null 2>&1 && cmake --build build/release -j4 > ../ladder/build-main.txt 2>&1; echo "main built: $?"
  build/release/tools/wrapper/oshcc -O2 -o ../ladder/get-pieces-main ../ladder/get-pieces.c; echo "probe main: $?"' >> "$LOG"
for i in 1 2 3; do
  for t in main branch; do
    echo "-- $t run $i" >> "$LOG"
    ssh pi-login "cd osoe && srun -K1 --cpu-bind=none -N2 --ntasks-per-node=1 build/ladder/get-pieces-$t 21" 2>&1 |
      grep '^bytes=' >> "$LOG"
  done
done
echo "== the branch's tests of gets on one node $(date +%H:%M:%S)" >> "$LOG"
ssh pi-login "cd osoe && srun -N1 -n1 -c4 ctest --test-dir build/release -j4 --timeout 300 --output-on-failure -R '^shmem\.(get|iget|rma|multiple|context|landing|stopped-bulk|internode|lanes\.threads)'" \
  > "$OUT/pi-gets-tests.txt" 2>&1
grep -E "tests passed|\(Failed\)|\(Timeout\)" "$OUT/pi-gets-tests.txt" >> "$LOG"
grep -E "a get of" "$OUT/pi-gets-tests.txt" >> "$LOG"
rm -rf "${OUT:?}/trees"
echo "end $(date +%H:%M:%S)" >> "$LOG"
