#!/bin/bash
# SPDX-License-Identifier: LGPL-3.0-or-later
# The blocking-gets read's fixes, checked before they are committed: the static
# checks, then the tests the fixes touch, in the debug preset; and the suite's
# counts, for the README.
# Usage: blocking-gets-read-check.sh TAR OUTDIR. Takes the VM lock.
exec 9>/tmp/osoe-vm.lock; flock -w 43200 9 || exit 1
TAR=$1; D=$2
mkdir -p $D; OUT=$D/check.txt; : > $OUT
T=/tmp/v02-blocking-gets-read; rm -rf "${T:?}"; mkdir -p $T; tar -C $T -xf $TAR; cd $T || exit 1
echo "== configure and build debug $(date +%H:%M:%S)" >> $OUT
cmake --preset debug -DOSOE_TEST_PINS=ON > $D/configure.txt 2>&1 || { echo "configure failed" >> $OUT; exit 1; }
cmake --build build/debug -j4 > $D/build.txt 2>&1 || { echo "BUILD FAILED" >> $OUT; grep -E "error" $D/build.txt | head -20 >> $OUT; exit 1; }
grep -E "warning:" $D/build.txt | sort -u | head >> $OUT
echo "== counts" >> $OUT
echo "all $(ctest --test-dir build/debug -N | grep -c 'Test *#')" >> $OUT
echo "internode $(ctest --test-dir build/debug -N -L internode | grep -c 'Test *#')" >> $OUT
echo "== static $(date +%H:%M:%S)" >> $OUT
ctest --test-dir build/debug -j4 --output-on-failure \
  -R '^shmem\.(memory-orders|strict-c11|boundary|static-link|exports|settings-documented|license-lines|single-node\.configure|version\.)' \
  > $D/static.full 2>&1
grep -E "tests passed|\(Failed\)|\(Timeout\)" $D/static.full >> $OUT
echo "== touched $(date +%H:%M:%S)" >> $OUT
ctest --test-dir build/debug -j4 --timeout 600 --output-on-failure \
  -R '^(shmem\.(get|iget|rma|multiple|context|landing|stopped|internode|lanes\.threads|stat|nbi|thread)|stat\.)' \
  > $D/touched.full 2>&1
grep -E "tests passed|\(Failed\)|\(Timeout\)|Not Run" $D/touched.full >> $OUT
grep -E "^ *[0-9]+/[0-9]+ Test" $D/touched.full | grep -E "get-no-progress|get-pieces|multiple|stat" >> $OUT
echo "== done $(date +%H:%M:%S)" >> $OUT
