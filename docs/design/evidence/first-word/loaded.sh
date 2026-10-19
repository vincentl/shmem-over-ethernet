#!/bin/bash
# SPDX-License-Identifier: LGPL-3.0-or-later
# loaded.sh BASE FIX N OUT LOAD: shmem.death-in-wait.barrier.L4 N times beside
# LOAD busy loops (repeat.sh), in debug: at the tree before the fix (BASE), as
# fixed (FIX), and as fixed with the await made to return at once (mutants.sh's
# no-await, which keeps the word taken right before the write), FIX restored
# and rebuilt after, all under the VM's lock.
exec 9>/tmp/osoe-vm.lock; flock -n 9 || { echo "VM LOCK HELD"; exit 1; }
export DIW_LOCK_HELD=1
BASE=$1 FIX=$2 N=$3 OUT=$4 LOAD=$5
HERE=$(cd "$(dirname "$0")" && pwd)
bash "$HERE/repeat.sh" "$BASE" debug "$N" "$OUT/before" "$LOAD" | tail -2
bash "$HERE/repeat.sh" "$FIX" debug "$N" "$OUT/fixed" "$LOAD" | tail -2
cd "$FIX" || exit 1
cp shmem/src/fatal.c "$OUT/fatal.c.orig" || exit 1
python3 -I - <<'PY'
p = 'shmem/src/fatal.c'
s = open(p).read()
a = ('    while (atomic_load_explicit(&first_word, memory_order_acquire) == BEING_SAID &&\n')
assert s.count(a) == 1
open(p, 'w').write(s.replace(a, '    while (0 && atomic_load_explicit(&first_word, memory_order_acquire) == BEING_SAID &&\n'))
PY
cmake --build build/debug -j4 > "$OUT/build-no-await.txt" 2>&1 || { echo "BUILD FAILED"; exit 1; }
bash "$HERE/repeat.sh" "$FIX" debug "$N" "$OUT/no-await" "$LOAD" | tail -2
cp "$OUT/fatal.c.orig" shmem/src/fatal.c
cmake --build build/debug -j4 > "$OUT/rebuild.txt" 2>&1; echo "restored and rebuilt $?"
echo "LOADED DONE $(date +%H:%M:%S)"
