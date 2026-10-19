#!/bin/bash
# SPDX-License-Identifier: LGPL-3.0-or-later
# verify.sh TAR DIR BASE N: a commit's git archive verified whole in the osoe VM
# (aarch64), as the batches' verification does (every preset built, the static
# checks, the counts, release, debug and asan but the models, the whole tsan
# preset and its gate, the single-node build's suite), under the VM's lock; then
# shmem.death-in-wait.barrier.L4 N times in release at BASE (built by setup.sh)
# and at DIR (repeat.sh).
exec 9>/tmp/osoe-vm.lock; flock -n 9 || { echo "VM LOCK HELD"; exit 1; }
export DIW_LOCK_HELD=1
TAR=$1 D=$2 BASE=$3 N=$4
HERE=$(cd "$(dirname "$0")" && pwd)
rm -rf "$D"; mkdir -p "$D"; tar -xf "$TAR" -C "$D"; cd "$D" || exit 1
T=$D
ok=1
for p in release debug asan tsan tier3; do
  cmake --preset $p > $T-cfg-$p.txt 2>&1 && cmake --build build/$p -j4 > $T-build-$p.txt 2>&1
  s=$?; echo "built $p $s warnings $(grep -c warning: $T-build-$p.txt)"; [ $s = 0 ] || ok=0
done
cmake -S . -B build/single-node -DOSOE_SINGLE_NODE=ON > $T-cfg-single.txt 2>&1 && cmake --build build/single-node -j4 > $T-build-single.txt 2>&1
s=$?; echo "built single-node $s warnings $(grep -c warning: $T-build-single.txt)"; [ $s = 0 ] || ok=0
[ $ok = 1 ] || { echo "BUILD FAILED"; exit 1; }
ctest --test-dir build/release -j4 --timeout 600 -R '^shmem\.(memory-orders|strict-c11|boundary|static-link|exports|settings-documented|settings-known|pass-expressions|whole-lines|license-lines|single-node\.configure|version\.)' > $T-static.txt 2>&1
echo "static: $(grep -E 'tests passed' $T-static.txt)"; grep -E '\(Failed\)|\(Timeout\)' $T-static.txt | head -5
n() { ctest --test-dir "$1" -N "${@:2}" | grep -c 'Test *#'; }
echo "counts: release $(n build/release -LE '^model$') internode $(n build/release -L internode) asan $(n build/asan) tsan $(n build/tsan) single-node $(n build/single-node)"
for p in release debug asan; do
  ctest --test-dir build/$p -j4 --timeout 600 -LE '^model$' > $T-$p.txt 2>&1
  echo "$p: $(grep -E 'tests passed' $T-$p.txt)"; grep -E '\(Failed\)|\(Timeout\)' $T-$p.txt | head -8
done
ctest --test-dir build/tsan -j4 --timeout 600 > $T-tsan.txt 2>&1
echo "tsan: $(grep -E 'tests passed' $T-tsan.txt)"; grep -E '\(Failed\)|\(Timeout\)' $T-tsan.txt | head -8
python3 -I cmake/thread-sanitizer-gate.py judge build/tsan/thread-sanitizer-reports --ctest ctest --build build/tsan --place build/tsan/osoe_thread_sanitizer_place > $T-gate.txt 2>&1
grep -E 'GATE|report files' $T-gate.txt | head -2
ctest --test-dir build/single-node -LE model -j4 --timeout 600 > $T-single.txt 2>&1
echo "single-node: $(grep -E 'tests passed' $T-single.txt)"; grep -E '\(Failed\)|\(Timeout\)' $T-single.txt | head -5
echo "oom kills so far: $(sudo -n dmesg | grep -c 'Out of memory')"
echo "VERIFY DONE $(date +%H:%M:%S)"
echo "== shmem.death-in-wait.barrier.L4 in release, $N runs before and after"
bash "$HERE/repeat.sh" "$BASE" release "$N" /tmp/diw-out/before-release | tail -2
bash "$HERE/repeat.sh" "$D" release "$N" /tmp/diw-out/after-release | tail -2
echo "RATES DONE $(date +%H:%M:%S)"
