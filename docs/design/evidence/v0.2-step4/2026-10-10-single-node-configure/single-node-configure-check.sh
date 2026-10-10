#!/bin/bash
# SPDX-License-Identifier: LGPL-3.0-or-later
# The single-node build configures again (docs/decisions/2026-10-10-single-node-configure.md):
# the release build's static checks with the new shmem.single-node.configure;
# a mutant, the guard back to ThreadSanitizer's alone, which must fail it; the
# check in the asan and tsan presets, configured only, each configuring its own
# single-node counterpart; and the single-node build itself, built and its
# suite run, as the merge point's final check did at 8e36752.
# usage: single-node-configure-check.sh TAR OUTDIR. Takes the VM lock.
exec 9>/tmp/osoe-vm.lock; flock -w 43200 9 || exit 1
TAR=$1; D=$2; mkdir -p "$D"; OUT=$D/check.txt; : > "$OUT"
T=/tmp/single-node-configure-check; rm -rf "${T:?}"; mkdir -p "$T/tree" "$T/mutant"
tar -C "$T/tree" -xf "$TAR"; tar -C "$T/mutant" -xf "$TAR"
python3 - "$T/mutant/shmem/tests/CMakeLists.txt" <<'PYEOF'
import sys
p = sys.argv[1]; s = open(p).read()
a = 'if(OSOE_INTERNODE AND NOT CMAKE_C_FLAGS MATCHES "sanitize=thread")\n  osoe_tier2_map_test(helper-wake'
assert s.count(a) == 1
open(p, "w").write(s.replace(a, 'if(NOT CMAKE_C_FLAGS MATCHES "sanitize=thread")\n  osoe_tier2_map_test(helper-wake'))
PYEOF
cd "$T/tree" || exit 1
echo "== static checks, release $(date +%H:%M:%S)" >> "$OUT"
cmake --preset release > /dev/null 2>&1 && cmake --build build/release -j4 > "$D/build-release.txt" 2>&1 \
  || { echo "BUILD FAILED release" >> "$OUT"; exit 1; }
echo "release warnings: $(grep -c 'warning:' "$D/build-release.txt")" >> "$OUT"
ctest --test-dir build/release -j4 --output-on-failure \
  -R '^shmem\.(memory-orders|strict-c11|boundary|static-link|exports|settings-documented|license-lines|single-node\.configure|version\.)' \
  > "$D/static.full" 2>&1
grep -E "tests passed|\(Failed\)" "$D/static.full" >> "$OUT"
echo "release tests: $(ctest --test-dir build/release -N | grep -c 'Test *#')" >> "$OUT"
echo "== the mutant, configured only $(date +%H:%M:%S)" >> "$OUT"
( cd "$T/mutant" && cmake --preset release > /dev/null 2>&1 ) || echo "mutant configure failed" >> "$OUT"
ctest --test-dir "$T/mutant/build/release" --output-on-failure -R '^shmem\.single-node\.configure$' \
  > "$D/mutant.full" 2>&1
grep -E "tests passed|\(Failed\)|Can not find test" "$D/mutant.full" >> "$OUT"
for p in asan tsan; do
  echo "== $p, configured only $(date +%H:%M:%S)" >> "$OUT"
  cmake --preset $p > /dev/null 2>&1 || { echo "configure $p failed" >> "$OUT"; continue; }
  ctest --test-dir build/$p --output-on-failure -R '^shmem\.single-node\.configure$' > "$D/$p.full" 2>&1
  grep -E "tests passed|\(Failed\)" "$D/$p.full" >> "$OUT"
  rm -rf build/$p
done
echo "== the single-node build $(date +%H:%M:%S)" >> "$OUT"
cmake -S . -B build/single-node -DCMAKE_BUILD_TYPE=Debug -DOSOE_SINGLE_NODE=ON > "$D/configure-single-node.txt" 2>&1 \
  && cmake --build build/single-node -j4 > "$D/build-single-node.txt" 2>&1 \
  || { echo "BUILD FAILED single-node" >> "$OUT"; exit 1; }
echo "warnings: $(grep -c 'warning:' "$D/build-single-node.txt")" >> "$OUT"
ctest --test-dir build/single-node -j4 -LE '^model$' --timeout 600 --output-on-failure > "$D/single-node.full" 2>&1
grep -E "tests passed|\(Failed\)" "$D/single-node.full" >> "$OUT"
cd /; rm -rf "${T:?}"
echo "DONE $(date +%H:%M:%S)" >> "$OUT"
