#!/bin/bash
# SPDX-License-Identifier: LGPL-3.0-or-later
# P9's mutants: each must fail shmem.team-claim.1024 and .200 (the 40's one word cannot tell).
cd /tmp/p9 || exit 1
cp shmem/src/node.c /tmp/p9-node.c.orig
run() {
  name=$1; shift
  cp /tmp/p9-node.c.orig shmem/src/node.c
  python3 -I -c "import sys; p='shmem/src/node.c'; s=open(p).read(); a,b=sys.argv[1],sys.argv[2]; assert s.count(a)==1, a; open(p,'w').write(s.replace(a,b))" "$1" "$2" || { echo "$name: PATTERN NOT FOUND"; return; }
  cmake --build build/release --target shmem_test_team-claim -j4 > /tmp/p9-mutant-$name-build.txt 2>&1 || { echo "$name: BUILD FAILED"; return; }
  ctest --test-dir build/release -R 'shmem\.team-claim\.' --timeout 60 > /tmp/p9-mutant-$name.txt 2>&1
  echo "$name: $(grep -E 'tests passed' /tmp/p9-mutant-$name.txt); failed: $(grep -oE 'shmem\.team-claim\.[0-9]+ .*\(Failed\)' /tmp/p9-mutant-$name.txt | awk '{print $1}' | tr '\n' ' ')"
}
run no-move 'lost = bits != seen && !moved;' 'lost = false; (void)seen;'
run no-wrap 'w = (w == 0 ? words : w) - 1;' 'if (w == 0) break; w--;'
run local-start 'w = words - 1, left = words;' 'w = local_id % words, left = words;'
cp /tmp/p9-node.c.orig shmem/src/node.c
cmake --build build/release --target shmem_test_team-claim -j4 > /tmp/p9-mutant-restored-build.txt 2>&1
ctest --test-dir build/release -R 'shmem\.team-claim\.' --timeout 60 > /tmp/p9-mutant-restored.txt 2>&1
echo "restored: $(grep -E 'tests passed' /tmp/p9-mutant-restored.txt)"
