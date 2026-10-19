#!/bin/bash
# SPDX-License-Identifier: LGPL-3.0-or-later
# H23's final tree (c87e39c1) again: every preset rebuilt, the static checks,
# the tests the boundary's fix and the counts touch, the tsan internode count,
# then the internode tests at two helpers and the mesh's notice seen.
cd /tmp/h23-verify || exit 1
T=/tmp/h23-final; ok=1
for p in release debug asan tsan tier3 single-node; do
  cmake --build build/$p -j4 > $T-build-$p.txt 2>&1; s=$?
  echo "rebuilt $p $s warnings $(grep -c warning: $T-build-$p.txt)"; [ $s = 0 ] || ok=0
done
[ $ok = 1 ] || { echo "BUILD FAILED"; exit 1; }
ctest --test-dir build/release -j4 --timeout 600 -R '^shmem\.(memory-orders|strict-c11|boundary|static-link|exports|settings-documented|settings-known|pass-expressions|whole-lines|license-lines|single-node\.configure|version\.)' > $T-static.txt 2>&1
echo "static: $(grep -E 'tests passed' $T-static.txt)"; grep -E '\(Failed\)|\(Timeout\)' $T-static.txt | head -5
for p in release debug asan tsan single-node; do
  ctest --test-dir build/$p -j4 --timeout 600 -LE '^model$' -R 'mesh|start-up|listener|rendezvous|wire|bootstrap|job\.secret' > $T-$p.txt 2>&1
  echo "$p affected: $(grep -E 'tests passed' $T-$p.txt)"; grep -E '\(Failed\)|\(Timeout\)' $T-$p.txt | head -5
done
n() { ctest --test-dir "$1" -N "${@:2}" | grep -c 'Test *#'; }
echo "internode: release $(n build/release -L internode), tsan $(n build/tsan -L internode)"
echo "FINAL DONE $(date +%H:%M:%S)"
/tmp/h23-extra.sh
