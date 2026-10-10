#!/bin/bash
# SPDX-License-Identifier: LGPL-3.0-or-later
# The README's "Building" numbers at v0.2 step 4's merge point (the review's
# readiness Q5-2 and portability P2), on an idle VM, in order:
#  1. the release workflow as the README gives it - configure, build one file at
#     a time, the tests one at a time - each timed;
#  2. the counts: the full build's tests, those labelled internode, and the
#     single-node build's (configured beside it, not built);
#  3. the models four at a time, as the README gives them, each test's time,
#     with every model process's resident memory sampled each second;
#  4. a container's defaults emulated: a private 64 MiB tmpfs on /dev/shm in a
#     mount namespace, and io_uring refused by the tree's deny-io-uring, the
#     release suite one test at a time - how many fail, and how.
# usage: building.sh TAR OUTDIR. Takes the VM lock.
exec 9>/tmp/osoe-vm.lock; flock -w 21600 9 || exit 1
TAR=$1; D=$2; mkdir -p "$D"; OUT=$D/building.txt; : > "$OUT"
T=/tmp/building-measure; rm -rf "${T:?}"; mkdir -p "$T"; tar -C "$T" -xf "$TAR"; cd "$T" || exit 1
now() { date +%s.%N; }
took() { echo "$1 $2" | awk '{printf "%.0f", $2 - $1}'; }
echo "== start $(date +%H:%M:%S), load $(cut -d' ' -f1-3 /proc/loadavg), $(nproc) cores" >> "$OUT"

a=$(now); cmake --preset release > "$D/configure.txt" 2>&1 || { echo "configure failed" >> "$OUT"; exit 1; }
b=$(now); cmake --build --preset release > "$D/build.txt" 2>&1 || { echo "BUILD FAILED" >> "$OUT"; exit 1; }
c=$(now); ctest --preset release > "$D/suite.txt" 2>&1; rc=$?
e=$(now)
echo "configure $(took $a $b) s, build one file at a time $(took $b $c) s, tests one at a time $(took $c $e) s (exit $rc)" >> "$OUT"
grep -E "tests passed|Total Test time" "$D/suite.txt" >> "$OUT"

echo "== counts $(date +%H:%M:%S)" >> "$OUT"
echo "full build: $(ctest --test-dir build/release -N | sed -n 's/^Total Tests: //p') tests," \
     "$(ctest --test-dir build/release -N -LE '^model$' | sed -n 's/^Total Tests: //p') without the models," \
     "$(ctest --test-dir build/release -N -L internode | sed -n 's/^Total Tests: //p') internode," \
     "$(ctest --test-dir build/release -N -L '^model$' | sed -n 's/^Total Tests: //p') models" >> "$OUT"
cmake -S . -B build/single-node -DCMAKE_BUILD_TYPE=Debug -DOSOE_SINGLE_NODE=ON > "$D/single-node-configure.txt" 2>&1 \
  && echo "single-node build: $(ctest --test-dir build/single-node -N -LE '^model$' | sed -n 's/^Total Tests: //p') tests without the models" >> "$OUT" \
  || echo "single-node configure failed" >> "$OUT"
rm -rf build/single-node

echo "== models four at a time $(date +%H:%M:%S)" >> "$OUT"
( while sleep 1; do ps -eo rss=,args= | grep -E 'modelcheck/[a-z_]*\.py' | grep -v grep; done ) > "$D/models-rss.txt" 2>/dev/null &
sampler=$!
a=$(now); ctest --preset models -j4 > "$D/models.txt" 2>&1; rc=$?; e=$(now)
kill "$sampler" 2>/dev/null; wait "$sampler" 2>/dev/null
echo "models -j4: $(took $a $e) s (exit $rc)" >> "$OUT"
grep -E "tests passed" "$D/models.txt" >> "$OUT"
echo "the five longest:" >> "$OUT"
sed -n 's/^.*Test *#[0-9]*: \(model[^ ]*\) .*Passed *\([0-9.]*\) sec.*$/\2 \1/p' "$D/models.txt" | sort -rn | head -5 >> "$OUT"
echo "the five largest, resident MiB at the most:" >> "$OUT"
awk '{rss=$1; $1=""; if (rss > most[$0]) most[$0] = rss} END {for (k in most) printf "%.0f%s\n", most[k] / 1024, k}' \
  "$D/models-rss.txt" | sort -rn | head -5 | cut -c1-200 >> "$OUT"

echo "== a container's defaults $(date +%H:%M:%S)" >> "$OUT"
DENY=$(find build/release -name deny-io-uring -type f -perm -u+x | head -1)
if [ -z "$DENY" ]; then
  echo "no deny-io-uring" >> "$OUT"
else
  sudo unshare -m --propagation private bash -c "
    mount -t tmpfs -o size=64m tmpfs /dev/shm || exit 1
    exec setpriv --reuid=$(id -u) --regid=$(id -g) --init-groups env HOME=$HOME PATH=$PATH \
      $DENY setup ctest --test-dir build/release -LE '^model\$' --timeout 600
  " > "$D/container.full" 2>&1
  grep -E "tests passed" "$D/container.full" >> "$OUT"
  sed -n 's/^[[:space:]]*[0-9][0-9]* - \([^ ]*\) (.*$/\1/p' "$D/container.full" | sort > "$D/container-failed.txt"
  echo "failed: $(wc -l < "$D/container-failed.txt")" >> "$OUT"
  echo "lines naming /dev/shm: $(grep -c '/dev/shm has' "$D/container.full")," \
       "naming io_uring: $(grep -ci 'io_uring' "$D/container.full")" >> "$OUT"
fi
cd /; rm -rf "${T:?}"
echo "DONE $(date +%H:%M:%S)" >> "$OUT"
