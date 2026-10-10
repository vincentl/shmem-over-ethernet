#!/bin/bash
# SPDX-License-Identifier: LGPL-3.0-or-later
# The tip that merges, after the full verification of 4f1014d: what changed
# since is shmem.helper-wake's retries, comments and documents. So: the static
# checks (the licence check over the evidence the record adds among them); the
# four presets built, with any warning; shmem.helper-wake three times in each,
# at its own two helpers; and the single-node build, which the full
# verification does not build, configured, built and its suite run - the one
# build SHMEM_INFO's note compiles in (the fixes' adversarial read, N10).
# usage: verify-final.sh TAR OUTDIR. Takes the VM lock.
exec 9>/tmp/osoe-vm.lock; flock -w 21600 9 || exit 1
TAR=$1; D=$2; mkdir -p "$D"; OUT=$D/verify.txt; : > "$OUT"
T=/tmp/v02-step4-final; rm -rf "${T:?}"; mkdir -p "$T"; tar -C "$T" -xf "$TAR"; cd "$T" || exit 1
echo "== static checks $(date +%H:%M:%S)" >> "$OUT"
cmake --preset release > /dev/null 2>&1 && cmake --build build/release -j4 > "$D/build-release.txt" 2>&1 \
  || { echo "BUILD FAILED release" >> "$OUT"; exit 1; }
ctest --test-dir build/release -j4 --output-on-failure \
  -R '^shmem\.(memory-orders|strict-c11|boundary|static-link|exports|settings-documented|license-lines|version\.)' \
  > "$D/static.full" 2>&1
grep -E "tests passed|\(Failed\)" "$D/static.full" >> "$OUT"
for p in debug release asan tsan; do
  echo "== $p $(date +%H:%M:%S)" >> "$OUT"
  if [ $p != release ]; then
    cmake --preset $p > /dev/null 2>&1 && cmake --build build/$p -j4 > "$D/build-$p.txt" 2>&1 \
      || { echo "BUILD FAILED $p" >> "$OUT"; continue; }
  fi
  echo "warnings: $(grep -c 'warning:' "$D/build-$p.txt")" >> "$OUT"
  [ $p = tsan ] && { rm -rf build/tsan; continue; }   # helper-wake is not built under ThreadSanitizer
  ctest --test-dir build/$p --repeat until-fail:3 -R '^shmem\.helper-wake\.0-1-2-3$' --output-on-failure \
    > "$D/helper-wake-$p.full" 2>&1
  grep -E "tests passed|\(Failed\)" "$D/helper-wake-$p.full" >> "$OUT"
  [ $p != release ] && rm -rf build/$p
done
echo "== the single-node build $(date +%H:%M:%S)" >> "$OUT"
cmake -S . -B build/single-node -DCMAKE_BUILD_TYPE=Debug -DOSOE_SINGLE_NODE=ON > "$D/configure-single-node.txt" 2>&1 \
  && cmake --build build/single-node -j4 > "$D/build-single-node.txt" 2>&1 \
  || { echo "BUILD FAILED single-node" >> "$OUT"; exit 1; }
echo "warnings: $(grep -c 'warning:' "$D/build-single-node.txt")" >> "$OUT"
ctest --test-dir build/single-node -j4 -LE '^model$' --timeout 600 --output-on-failure > "$D/single-node.full" 2>&1
grep -E "tests passed|\(Failed\)" "$D/single-node.full" >> "$OUT"
build/single-node/tools/launch/osoe-launch -n 1 -t 60 -e 0:SHMEM_INFO=1 -- \
  "$(find build/single-node -name shmem_test_init -type f -perm -u+x | head -1)" > "$D/single-node-info.txt" 2>&1 || true
grep -A4 'single-node build' "$D/single-node-info.txt" >> "$OUT" || echo "no single-node note seen" >> "$OUT"
cd /; rm -rf "${T:?}"
echo "DONE $(date +%H:%M:%S)" >> "$OUT"
