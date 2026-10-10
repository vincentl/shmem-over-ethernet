#!/bin/bash
# SPDX-License-Identifier: LGPL-3.0-or-later
# v0.2 step 4's merge point: the release preset with the floor pins on at two
# helpers and at three - the presets and counts last night's verification of
# 232278a (verify-14, the code main has at c897948) did not run: release ran
# there at none alone. usage: presets-helpers.sh TAR OUTDIR. Takes the VM lock.
exec 9>/tmp/osoe-vm.lock; flock -w 21600 9 || exit 1
TAR=$1; D=$2; mkdir -p "$D"; OUT=$D/presets.txt; : > "$OUT"
T=/tmp/v02-merge-point-presets; rm -rf "${T:?}"; mkdir -p "$T"; tar -C "$T" -xf "$TAR"; cd "$T" || exit 1
echo "== build release $(date +%H:%M:%S)" >> "$OUT"
cmake --preset release -DOSOE_TEST_PINS=ON > /dev/null 2>&1 || { echo "configure failed" >> "$OUT"; exit 1; }
cmake --build build/release -j4 > "$D/build-release.txt" 2>&1 || { echo "BUILD FAILED" >> "$OUT"; exit 1; }
grep -E "warning:" "$D/build-release.txt" | sort -u | head >> "$OUT"
for h in 2 3; do
  echo "== release OSOE_PROGRESS_HELPERS=$h $(date +%H:%M:%S)" >> "$OUT"
  OSOE_PROGRESS_HELPERS=$h ctest --test-dir build/release -j4 --timeout 600 --output-on-failure \
    -LE '^model$' > "$D/release-$h.full" 2>&1
  grep -E "tests passed|\*\*\*Failed|\*\*\*Timeout|Not Run|\(Failed\)|\(Timeout\)" "$D/release-$h.full" >> "$OUT"
done
cd /; rm -rf "${T:?}"
echo "DONE $(date +%H:%M:%S)" >> "$OUT"
